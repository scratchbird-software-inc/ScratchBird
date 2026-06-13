// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/descriptor.hpp"
#include "scratchbird/engine/execution_type_descriptor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine {

// SEARCH_KEY: EDR_EXECUTION_VALUE_STATE
enum class ExecutionValueState : std::uint8_t {
  value = 0,
  sql_null = 1,
  missing = 2,
  default_requested = 3,
  unknown = 4,
  error = 5,
  lob_handle = 6,
  protected_value = 7
};

constexpr bool ExecutionValueStateIsSqlNull(ExecutionValueState state) noexcept {
  return state == ExecutionValueState::sql_null;
}

constexpr bool ExecutionValueStateHasPayload(ExecutionValueState state) noexcept {
  switch (state) {
    case ExecutionValueState::value:
    case ExecutionValueState::error:
    case ExecutionValueState::lob_handle:
    case ExecutionValueState::protected_value:
      return true;
    case ExecutionValueState::sql_null:
    case ExecutionValueState::missing:
    case ExecutionValueState::default_requested:
    case ExecutionValueState::unknown:
      return false;
  }
  return false;
}

struct ExecutionValue {
  Descriptor descriptor;
  bool is_null = true;
  std::vector<std::uint8_t> encoded_value;
  ExecutionValueState state = ExecutionValueState::sql_null;

  bool isSqlNull() const noexcept {
    return ExecutionValueStateIsSqlNull(state) ||
           (state == ExecutionValueState::value && is_null);
  }

  bool isNull() const noexcept {
    return isSqlNull();
  }

  bool hasPayload() const noexcept {
    return ExecutionValueStateHasPayload(state) && !isSqlNull();
  }

  void setState(ExecutionValueState new_state) noexcept {
    state = new_state;
    is_null = ExecutionValueStateIsSqlNull(new_state);
  }
};

// SEARCH_KEY: EDR_PLAIN_VALUE_PAYLOAD_SBC1
inline constexpr std::uint8_t kPlainValuePayloadMagic0 = 'S';
inline constexpr std::uint8_t kPlainValuePayloadMagic1 = 'B';
inline constexpr std::uint8_t kPlainValuePayloadMagic2 = 'C';
inline constexpr std::uint8_t kPlainValuePayloadMagic3 = '1';
inline constexpr std::uint8_t kPlainValuePayloadMajorVersion = 1;
inline constexpr std::uint8_t kPlainValuePayloadMinorVersion = 0;
inline constexpr std::size_t kPlainValuePayloadHeaderSize = 16;

enum class PlainValuePayloadStatus : std::uint8_t {
  ok = 0,
  truncated_header = 1,
  invalid_magic = 2,
  unsupported_version = 3,
  invalid_reserved = 4,
  invalid_state = 5,
  payload_length_mismatch = 6,
  payload_not_allowed = 7,
  payload_length_overflow = 8
};

constexpr std::string_view PlainValuePayloadStatusName(
    PlainValuePayloadStatus status) noexcept {
  switch (status) {
    case PlainValuePayloadStatus::ok:
      return "ok";
    case PlainValuePayloadStatus::truncated_header:
      return "truncated_header";
    case PlainValuePayloadStatus::invalid_magic:
      return "invalid_magic";
    case PlainValuePayloadStatus::unsupported_version:
      return "unsupported_version";
    case PlainValuePayloadStatus::invalid_reserved:
      return "invalid_reserved";
    case PlainValuePayloadStatus::invalid_state:
      return "invalid_state";
    case PlainValuePayloadStatus::payload_length_mismatch:
      return "payload_length_mismatch";
    case PlainValuePayloadStatus::payload_not_allowed:
      return "payload_not_allowed";
    case PlainValuePayloadStatus::payload_length_overflow:
      return "payload_length_overflow";
  }
  return "unknown_status";
}

constexpr bool PlainValuePayloadStateCodeIsValid(std::uint8_t code) noexcept {
  switch (code) {
    case static_cast<std::uint8_t>(ExecutionValueState::value):
    case static_cast<std::uint8_t>(ExecutionValueState::sql_null):
    case static_cast<std::uint8_t>(ExecutionValueState::missing):
    case static_cast<std::uint8_t>(ExecutionValueState::default_requested):
    case static_cast<std::uint8_t>(ExecutionValueState::unknown):
    case static_cast<std::uint8_t>(ExecutionValueState::error):
    case static_cast<std::uint8_t>(ExecutionValueState::lob_handle):
    case static_cast<std::uint8_t>(ExecutionValueState::protected_value):
      return true;
    default:
      return false;
  }
}

struct PlainValuePayload {
  ExecutionValueState state = ExecutionValueState::sql_null;
  std::vector<std::uint8_t> payload;
};

struct PlainValuePayloadEncodeResult {
  PlainValuePayloadStatus status = PlainValuePayloadStatus::ok;
  std::vector<std::uint8_t> encoded;

  bool ok() const noexcept {
    return status == PlainValuePayloadStatus::ok;
  }
};

struct PlainValuePayloadDecodeResult {
  PlainValuePayloadStatus status = PlainValuePayloadStatus::ok;
  PlainValuePayload value;

  bool ok() const noexcept {
    return status == PlainValuePayloadStatus::ok;
  }
};

inline PlainValuePayloadEncodeResult serializePlainValue(
    ExecutionValueState state,
    std::span<const std::uint8_t> payload) {
  if (!ExecutionValueStateHasPayload(state) && !payload.empty()) {
    return {PlainValuePayloadStatus::payload_not_allowed, {}};
  }

  const auto payload_size = static_cast<std::uint64_t>(payload.size());
  if (static_cast<std::size_t>(payload_size) != payload.size()) {
    return {PlainValuePayloadStatus::payload_length_overflow, {}};
  }

  PlainValuePayloadEncodeResult result;
  result.encoded.reserve(kPlainValuePayloadHeaderSize + payload.size());
  result.encoded.push_back(kPlainValuePayloadMagic0);
  result.encoded.push_back(kPlainValuePayloadMagic1);
  result.encoded.push_back(kPlainValuePayloadMagic2);
  result.encoded.push_back(kPlainValuePayloadMagic3);
  result.encoded.push_back(kPlainValuePayloadMajorVersion);
  result.encoded.push_back(kPlainValuePayloadMinorVersion);
  result.encoded.push_back(static_cast<std::uint8_t>(state));
  result.encoded.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    result.encoded.push_back(
        static_cast<std::uint8_t>((payload_size >> shift) & 0xffu));
  }
  result.encoded.insert(result.encoded.end(), payload.begin(), payload.end());
  return result;
}

inline PlainValuePayloadEncodeResult serializePlainValue(
    const ExecutionValue& value) {
  const ExecutionValueState state =
      value.isSqlNull() ? ExecutionValueState::sql_null : value.state;
  if (!ExecutionValueStateHasPayload(state)) {
    return serializePlainValue(state, std::span<const std::uint8_t>{});
  }
  return serializePlainValue(state, value.encoded_value);
}

inline PlainValuePayloadDecodeResult parsePlainValuePayload(
    std::span<const std::uint8_t> encoded) {
  if (encoded.size() < kPlainValuePayloadHeaderSize) {
    return {PlainValuePayloadStatus::truncated_header, {}};
  }
  if (encoded[0] != kPlainValuePayloadMagic0 ||
      encoded[1] != kPlainValuePayloadMagic1 ||
      encoded[2] != kPlainValuePayloadMagic2 ||
      encoded[3] != kPlainValuePayloadMagic3) {
    return {PlainValuePayloadStatus::invalid_magic, {}};
  }
  if (encoded[4] != kPlainValuePayloadMajorVersion ||
      encoded[5] != kPlainValuePayloadMinorVersion) {
    return {PlainValuePayloadStatus::unsupported_version, {}};
  }
  if (encoded[7] != 0) {
    return {PlainValuePayloadStatus::invalid_reserved, {}};
  }
  if (!PlainValuePayloadStateCodeIsValid(encoded[6])) {
    return {PlainValuePayloadStatus::invalid_state, {}};
  }

  std::uint64_t payload_length = 0;
  for (std::size_t index = 8; index < kPlainValuePayloadHeaderSize; ++index) {
    payload_length = (payload_length << 8) | encoded[index];
  }
  if (payload_length >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return {PlainValuePayloadStatus::payload_length_mismatch, {}};
  }
  const auto available_payload =
      static_cast<std::uint64_t>(encoded.size() - kPlainValuePayloadHeaderSize);
  if (payload_length != available_payload) {
    return {PlainValuePayloadStatus::payload_length_mismatch, {}};
  }

  const auto state = static_cast<ExecutionValueState>(encoded[6]);
  if (!ExecutionValueStateHasPayload(state) && payload_length != 0) {
    return {PlainValuePayloadStatus::payload_not_allowed, {}};
  }

  PlainValuePayloadDecodeResult result;
  result.value.state = state;
  result.value.payload.assign(encoded.begin() + kPlainValuePayloadHeaderSize,
                              encoded.end());
  return result;
}

inline PlainValuePayloadDecodeResult deserializePlainValue(
    std::span<const std::uint8_t> encoded) {
  return parsePlainValuePayload(encoded);
}

// SEARCH_KEY: EDR_EXECUTION_DATA_PACKET_CONTRACT
inline constexpr std::uint8_t kExecutionDataPacketMajorVersion = 1;
inline constexpr std::uint8_t kExecutionDataPacketMinorVersion = 0;

struct ExecutionDataPacketSlot {
  std::uint32_t descriptor_index = 0;
  ExecutionValueState state = ExecutionValueState::sql_null;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_size = 0;
};

struct ExecutionDataPacket {
  std::uint8_t major_version = kExecutionDataPacketMajorVersion;
  std::uint8_t minor_version = kExecutionDataPacketMinorVersion;
  std::vector<ExecutionTypeDescriptor> descriptor_table;
  std::vector<ExecutionDataPacketSlot> slot_table;
  std::vector<std::uint8_t> payload_area;
};

enum class ExecutionDataPacketStatus : std::uint8_t {
  ok = 0,
  unsupported_version = 1,
  descriptor_table_required = 2,
  descriptor_index_out_of_range = 3,
  descriptor_missing_uuid = 4,
  descriptor_missing_epoch = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  invalid_value_state = 8,
  payload_not_allowed = 9,
  payload_range_overflow = 10,
  payload_range_out_of_bounds = 11,
  payload_range_overlap = 12,
  payload_unreferenced = 13
};

constexpr std::string_view ExecutionDataPacketStatusName(
    ExecutionDataPacketStatus status) noexcept {
  switch (status) {
    case ExecutionDataPacketStatus::ok:
      return "ok";
    case ExecutionDataPacketStatus::unsupported_version:
      return "unsupported_version";
    case ExecutionDataPacketStatus::descriptor_table_required:
      return "descriptor_table_required";
    case ExecutionDataPacketStatus::descriptor_index_out_of_range:
      return "descriptor_index_out_of_range";
    case ExecutionDataPacketStatus::descriptor_missing_uuid:
      return "descriptor_missing_uuid";
    case ExecutionDataPacketStatus::descriptor_missing_epoch:
      return "descriptor_missing_epoch";
    case ExecutionDataPacketStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionDataPacketStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionDataPacketStatus::invalid_value_state:
      return "invalid_value_state";
    case ExecutionDataPacketStatus::payload_not_allowed:
      return "payload_not_allowed";
    case ExecutionDataPacketStatus::payload_range_overflow:
      return "payload_range_overflow";
    case ExecutionDataPacketStatus::payload_range_out_of_bounds:
      return "payload_range_out_of_bounds";
    case ExecutionDataPacketStatus::payload_range_overlap:
      return "payload_range_overlap";
    case ExecutionDataPacketStatus::payload_unreferenced:
      return "payload_unreferenced";
  }
  return "unknown_status";
}

struct ExecutionDataPacketValidationResult {
  ExecutionDataPacketStatus status = ExecutionDataPacketStatus::ok;
  std::size_t slot_index = 0;
  std::size_t descriptor_index = 0;

  bool ok() const noexcept {
    return status == ExecutionDataPacketStatus::ok;
  }
};

inline bool ExecutionDataPacketUuidIsNil(const Uuid& uuid) noexcept {
  for (const std::uint8_t byte : uuid.bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

inline bool ExecutionDataPacketUuidEquals(const Uuid& left,
                                          const Uuid& right) noexcept {
  for (std::size_t index = 0; index < 16; ++index) {
    if (left.bytes[index] != right.bytes[index]) {
      return false;
    }
  }
  return true;
}

inline ExecutionDataPacketValidationResult ValidateExecutionDataPacketDescriptor(
    const ExecutionTypeDescriptor& descriptor,
    std::size_t descriptor_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.descriptor_uuid)) {
    return {ExecutionDataPacketStatus::descriptor_missing_uuid, 0,
            descriptor_index};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {ExecutionDataPacketStatus::descriptor_missing_epoch, 0,
            descriptor_index};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionDataPacketStatus::descriptor_not_authoritative, 0,
            descriptor_index};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionDataPacketStatus::descriptor_parser_dependent, 0,
            descriptor_index};
  }
  return {};
}

inline ExecutionDataPacketValidationResult ValidateExecutionDataPacket(
    const ExecutionDataPacket& packet) {
  if (packet.major_version != kExecutionDataPacketMajorVersion ||
      packet.minor_version != kExecutionDataPacketMinorVersion) {
    return {ExecutionDataPacketStatus::unsupported_version, 0, 0};
  }
  if (!packet.slot_table.empty() && packet.descriptor_table.empty()) {
    return {ExecutionDataPacketStatus::descriptor_table_required, 0, 0};
  }

  for (std::size_t descriptor_index = 0;
       descriptor_index < packet.descriptor_table.size(); ++descriptor_index) {
    const auto descriptor_result = ValidateExecutionDataPacketDescriptor(
        packet.descriptor_table[descriptor_index], descriptor_index);
    if (!descriptor_result.ok()) {
      return descriptor_result;
    }
  }

  std::vector<bool> payload_coverage(packet.payload_area.size(), false);
  for (std::size_t slot_index = 0; slot_index < packet.slot_table.size();
       ++slot_index) {
    const auto& slot = packet.slot_table[slot_index];
    if (slot.descriptor_index >= packet.descriptor_table.size()) {
      return {ExecutionDataPacketStatus::descriptor_index_out_of_range,
              slot_index, slot.descriptor_index};
    }
    if (!PlainValuePayloadStateCodeIsValid(static_cast<std::uint8_t>(slot.state))) {
      return {ExecutionDataPacketStatus::invalid_value_state, slot_index,
              slot.descriptor_index};
    }
    if (!ExecutionValueStateHasPayload(slot.state) && slot.payload_size != 0) {
      return {ExecutionDataPacketStatus::payload_not_allowed, slot_index,
              slot.descriptor_index};
    }
    if (!ExecutionValueStateHasPayload(slot.state) && slot.payload_offset != 0) {
      return {ExecutionDataPacketStatus::payload_range_out_of_bounds, slot_index,
              slot.descriptor_index};
    }
    if (slot.payload_size == 0) {
      if (slot.payload_offset != 0) {
        return {ExecutionDataPacketStatus::payload_range_out_of_bounds,
                slot_index, slot.descriptor_index};
      }
      continue;
    }
    if (slot.payload_offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return {ExecutionDataPacketStatus::payload_range_out_of_bounds, slot_index,
              slot.descriptor_index};
    }
    if (slot.payload_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return {ExecutionDataPacketStatus::payload_range_out_of_bounds, slot_index,
              slot.descriptor_index};
    }
    if (slot.payload_offset + slot.payload_size < slot.payload_offset) {
      return {ExecutionDataPacketStatus::payload_range_overflow, slot_index,
              slot.descriptor_index};
    }
    const auto begin = static_cast<std::size_t>(slot.payload_offset);
    const auto size = static_cast<std::size_t>(slot.payload_size);
    if (begin > packet.payload_area.size() ||
        size > packet.payload_area.size() - begin) {
      return {ExecutionDataPacketStatus::payload_range_out_of_bounds, slot_index,
              slot.descriptor_index};
    }
    for (std::size_t index = begin; index < begin + size; ++index) {
      if (payload_coverage[index]) {
        return {ExecutionDataPacketStatus::payload_range_overlap, slot_index,
                slot.descriptor_index};
      }
      payload_coverage[index] = true;
    }
  }

  for (const bool covered : payload_coverage) {
    if (!covered) {
      return {ExecutionDataPacketStatus::payload_unreferenced, 0, 0};
    }
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_ROW_BATCH_CONTRACT
inline constexpr std::uint32_t kExecutionRowBatchHardMaxRows = 65536;

struct ExecutionRowShapeColumn {
  std::uint32_t descriptor_index = 0;
  bool nullable = true;
};

struct ExecutionRowShapeDescriptor {
  std::vector<ExecutionRowShapeColumn> columns;
};

struct ExecutionRow {
  std::vector<std::uint32_t> slot_indexes;
  std::vector<ExecutionValueState> value_state_bitmap;
  std::vector<bool> null_bitmap;
};

struct ExecutionRowBatch {
  std::uint64_t batch_sequence = 0;
  std::uint64_t first_row_ordinal = 0;
  bool final_batch = false;
  std::uint32_t max_rows = kExecutionRowBatchHardMaxRows;
  ExecutionRowShapeDescriptor row_shape;
  ExecutionDataPacket data_packet;
  std::vector<ExecutionRow> rows;
};

enum class ExecutionRowBatchStatus : std::uint8_t {
  ok = 0,
  invalid_data_packet = 1,
  max_rows_zero = 2,
  max_rows_exceeds_limit = 3,
  row_count_exceeds_limit = 4,
  row_shape_required = 5,
  row_shape_descriptor_out_of_range = 6,
  row_width_mismatch = 7,
  row_slot_index_out_of_range = 8,
  row_slot_descriptor_mismatch = 9,
  row_state_bitmap_mismatch = 10,
  row_null_bitmap_mismatch = 11,
  row_non_nullable_null = 12,
  row_invalid_value_state = 13
};

constexpr std::string_view ExecutionRowBatchStatusName(
    ExecutionRowBatchStatus status) noexcept {
  switch (status) {
    case ExecutionRowBatchStatus::ok:
      return "ok";
    case ExecutionRowBatchStatus::invalid_data_packet:
      return "invalid_data_packet";
    case ExecutionRowBatchStatus::max_rows_zero:
      return "max_rows_zero";
    case ExecutionRowBatchStatus::max_rows_exceeds_limit:
      return "max_rows_exceeds_limit";
    case ExecutionRowBatchStatus::row_count_exceeds_limit:
      return "row_count_exceeds_limit";
    case ExecutionRowBatchStatus::row_shape_required:
      return "row_shape_required";
    case ExecutionRowBatchStatus::row_shape_descriptor_out_of_range:
      return "row_shape_descriptor_out_of_range";
    case ExecutionRowBatchStatus::row_width_mismatch:
      return "row_width_mismatch";
    case ExecutionRowBatchStatus::row_slot_index_out_of_range:
      return "row_slot_index_out_of_range";
    case ExecutionRowBatchStatus::row_slot_descriptor_mismatch:
      return "row_slot_descriptor_mismatch";
    case ExecutionRowBatchStatus::row_state_bitmap_mismatch:
      return "row_state_bitmap_mismatch";
    case ExecutionRowBatchStatus::row_null_bitmap_mismatch:
      return "row_null_bitmap_mismatch";
    case ExecutionRowBatchStatus::row_non_nullable_null:
      return "row_non_nullable_null";
    case ExecutionRowBatchStatus::row_invalid_value_state:
      return "row_invalid_value_state";
  }
  return "unknown_status";
}

struct ExecutionRowBatchValidationResult {
  ExecutionRowBatchStatus status = ExecutionRowBatchStatus::ok;
  ExecutionDataPacketStatus packet_status = ExecutionDataPacketStatus::ok;
  std::size_t row_index = 0;
  std::size_t column_index = 0;

  bool ok() const noexcept {
    return status == ExecutionRowBatchStatus::ok;
  }
};

inline ExecutionRowBatchValidationResult ValidateExecutionRowBatch(
    const ExecutionRowBatch& batch) {
  const auto packet_result = ValidateExecutionDataPacket(batch.data_packet);
  if (!packet_result.ok()) {
    return {ExecutionRowBatchStatus::invalid_data_packet, packet_result.status,
            packet_result.slot_index, packet_result.descriptor_index};
  }
  if (batch.max_rows == 0) {
    return {ExecutionRowBatchStatus::max_rows_zero};
  }
  if (batch.max_rows > kExecutionRowBatchHardMaxRows) {
    return {ExecutionRowBatchStatus::max_rows_exceeds_limit};
  }
  if (batch.rows.size() > batch.max_rows ||
      batch.rows.size() > kExecutionRowBatchHardMaxRows) {
    return {ExecutionRowBatchStatus::row_count_exceeds_limit};
  }
  if (!batch.rows.empty() && batch.row_shape.columns.empty()) {
    return {ExecutionRowBatchStatus::row_shape_required};
  }

  for (std::size_t column_index = 0;
       column_index < batch.row_shape.columns.size(); ++column_index) {
    if (batch.row_shape.columns[column_index].descriptor_index >=
        batch.data_packet.descriptor_table.size()) {
      return {ExecutionRowBatchStatus::row_shape_descriptor_out_of_range,
              ExecutionDataPacketStatus::ok, 0, column_index};
    }
  }

  for (std::size_t row_index = 0; row_index < batch.rows.size(); ++row_index) {
    const auto& row = batch.rows[row_index];
    const auto width = batch.row_shape.columns.size();
    if (row.slot_indexes.size() != width ||
        row.value_state_bitmap.size() != width ||
        row.null_bitmap.size() != width) {
      return {ExecutionRowBatchStatus::row_width_mismatch,
              ExecutionDataPacketStatus::ok, row_index, 0};
    }

    for (std::size_t column_index = 0; column_index < width; ++column_index) {
      const auto slot_index = row.slot_indexes[column_index];
      if (slot_index >= batch.data_packet.slot_table.size()) {
        return {ExecutionRowBatchStatus::row_slot_index_out_of_range,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
      const auto& slot = batch.data_packet.slot_table[slot_index];
      if (slot.descriptor_index !=
          batch.row_shape.columns[column_index].descriptor_index) {
        return {ExecutionRowBatchStatus::row_slot_descriptor_mismatch,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
      if (!PlainValuePayloadStateCodeIsValid(
              static_cast<std::uint8_t>(row.value_state_bitmap[column_index]))) {
        return {ExecutionRowBatchStatus::row_invalid_value_state,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
      if (row.value_state_bitmap[column_index] != slot.state) {
        return {ExecutionRowBatchStatus::row_state_bitmap_mismatch,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
      const bool slot_is_sql_null = slot.state == ExecutionValueState::sql_null;
      if (row.null_bitmap[column_index] != slot_is_sql_null) {
        return {ExecutionRowBatchStatus::row_null_bitmap_mismatch,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
      if (slot_is_sql_null && !batch.row_shape.columns[column_index].nullable) {
        return {ExecutionRowBatchStatus::row_non_nullable_null,
                ExecutionDataPacketStatus::ok, row_index, column_index};
      }
    }
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_RESULT_ENVELOPE_CONTRACT
inline constexpr std::uint8_t kExecutionResultEnvelopeMajorVersion = 1;
inline constexpr std::uint8_t kExecutionResultEnvelopeMinorVersion = 0;

enum class ExecutionResultEnvelopeKind : std::uint8_t {
  row_set = 0,
  command_completion = 1,
  execution_summary = 2,
  diagnostic_only = 3,
  capability_report = 4
};

struct ResultColumnDescriptor {
  std::uint32_t ordinal = 0;
  ExecutionTypeDescriptor descriptor;
  std::string semantic_name;
  std::string native_rendering_name;
  std::string reference_rendering_name;
  bool nullable = true;
};

// SEARCH_KEY: EDR_EXECUTION_RELATION_DESCRIPTOR
enum class ExecutionRelationKind : std::uint8_t {
  cursor = 0,
  rowset = 1,
  table_value = 2,
  result_channel = 3,
  remote_fragment = 4
};

constexpr bool ExecutionRelationKindIsValid(
    ExecutionRelationKind kind) noexcept {
  switch (kind) {
    case ExecutionRelationKind::cursor:
    case ExecutionRelationKind::rowset:
    case ExecutionRelationKind::table_value:
    case ExecutionRelationKind::result_channel:
    case ExecutionRelationKind::remote_fragment:
      return true;
  }
  return false;
}

struct ExecutionRelationDescriptor {
  Uuid relation_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionRelationKind relation_kind = ExecutionRelationKind::rowset;
  std::string stable_name;
  std::vector<ResultColumnDescriptor> columns;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool snapshot_required = true;
  Uuid snapshot_uuid{};
  bool security_context_required = false;
  Uuid security_policy_uuid{};
  bool memory_policy_required = true;
  Uuid memory_policy_uuid{};
  std::uint64_t memory_policy_epoch = 1;
  Uuid coordinator_fragment_uuid{};
  Uuid worker_fragment_uuid{};
  std::uint32_t fragment_ordinal = 0;
};

enum class ExecutionRelationDescriptorStatus : std::uint8_t {
  ok = 0,
  descriptor_uuid_required = 1,
  descriptor_epoch_required = 2,
  stable_name_required = 3,
  descriptor_not_authoritative = 4,
  descriptor_parser_dependent = 5,
  relation_kind_invalid = 6,
  columns_required = 7,
  column_ordinal_mismatch = 8,
  column_descriptor_invalid = 9,
  column_rendering_metadata_required = 10,
  snapshot_uuid_required = 11,
  security_policy_uuid_required = 12,
  memory_policy_uuid_required = 13,
  memory_policy_epoch_required = 14,
  remote_fragment_uuid_required = 15
};

constexpr std::string_view ExecutionRelationDescriptorStatusName(
    ExecutionRelationDescriptorStatus status) noexcept {
  switch (status) {
    case ExecutionRelationDescriptorStatus::ok:
      return "ok";
    case ExecutionRelationDescriptorStatus::descriptor_uuid_required:
      return "descriptor_uuid_required";
    case ExecutionRelationDescriptorStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionRelationDescriptorStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionRelationDescriptorStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionRelationDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionRelationDescriptorStatus::relation_kind_invalid:
      return "relation_kind_invalid";
    case ExecutionRelationDescriptorStatus::columns_required:
      return "columns_required";
    case ExecutionRelationDescriptorStatus::column_ordinal_mismatch:
      return "column_ordinal_mismatch";
    case ExecutionRelationDescriptorStatus::column_descriptor_invalid:
      return "column_descriptor_invalid";
    case ExecutionRelationDescriptorStatus::column_rendering_metadata_required:
      return "column_rendering_metadata_required";
    case ExecutionRelationDescriptorStatus::snapshot_uuid_required:
      return "snapshot_uuid_required";
    case ExecutionRelationDescriptorStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionRelationDescriptorStatus::memory_policy_uuid_required:
      return "memory_policy_uuid_required";
    case ExecutionRelationDescriptorStatus::memory_policy_epoch_required:
      return "memory_policy_epoch_required";
    case ExecutionRelationDescriptorStatus::remote_fragment_uuid_required:
      return "remote_fragment_uuid_required";
  }
  return "unknown_status";
}

struct ExecutionRelationDescriptorValidationResult {
  ExecutionRelationDescriptorStatus status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionDataPacketStatus column_descriptor_status =
      ExecutionDataPacketStatus::ok;
  std::size_t column_index = 0;

  bool ok() const noexcept {
    return status == ExecutionRelationDescriptorStatus::ok;
  }
};

inline ExecutionRelationDescriptorValidationResult
ValidateExecutionRelationDescriptor(
    const ExecutionRelationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.relation_descriptor_uuid)) {
    return {ExecutionRelationDescriptorStatus::descriptor_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {ExecutionRelationDescriptorStatus::descriptor_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ExecutionRelationDescriptorStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionRelationDescriptorStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionRelationDescriptorStatus::descriptor_parser_dependent};
  }
  if (!ExecutionRelationKindIsValid(descriptor.relation_kind)) {
    return {ExecutionRelationDescriptorStatus::relation_kind_invalid};
  }
  if (descriptor.columns.empty()) {
    return {ExecutionRelationDescriptorStatus::columns_required};
  }
  for (std::size_t column_index = 0; column_index < descriptor.columns.size();
       ++column_index) {
    const auto& column = descriptor.columns[column_index];
    if (column.ordinal != column_index) {
      return {ExecutionRelationDescriptorStatus::column_ordinal_mismatch,
              ExecutionDataPacketStatus::ok, column_index};
    }
    const auto column_descriptor_result =
        ValidateExecutionDataPacketDescriptor(column.descriptor, column_index);
    if (!column_descriptor_result.ok()) {
      return {ExecutionRelationDescriptorStatus::column_descriptor_invalid,
              column_descriptor_result.status, column_index};
    }
    if (column.semantic_name.empty() || column.native_rendering_name.empty() ||
        column.reference_rendering_name.empty()) {
      return {
          ExecutionRelationDescriptorStatus::column_rendering_metadata_required,
          ExecutionDataPacketStatus::ok, column_index};
    }
  }
  if (descriptor.snapshot_required &&
      ExecutionDataPacketUuidIsNil(descriptor.snapshot_uuid)) {
    return {ExecutionRelationDescriptorStatus::snapshot_uuid_required};
  }
  if (descriptor.security_context_required &&
      ExecutionDataPacketUuidIsNil(descriptor.security_policy_uuid)) {
    return {ExecutionRelationDescriptorStatus::security_policy_uuid_required};
  }
  if (descriptor.memory_policy_required &&
      ExecutionDataPacketUuidIsNil(descriptor.memory_policy_uuid)) {
    return {ExecutionRelationDescriptorStatus::memory_policy_uuid_required};
  }
  if (descriptor.memory_policy_required &&
      descriptor.memory_policy_epoch == 0) {
    return {ExecutionRelationDescriptorStatus::memory_policy_epoch_required};
  }
  if (descriptor.relation_kind == ExecutionRelationKind::remote_fragment &&
      (ExecutionDataPacketUuidIsNil(descriptor.coordinator_fragment_uuid) ||
       ExecutionDataPacketUuidIsNil(descriptor.worker_fragment_uuid))) {
    return {ExecutionRelationDescriptorStatus::remote_fragment_uuid_required};
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_CURSOR_HANDLE
enum class PortalState : std::uint8_t {
  declared = 0,
  open = 1,
  fetching = 2,
  exhausted = 3,
  detached = 4,
  closed = 5,
  cleanup_pending = 6,
  error = 7
};

constexpr bool PortalStateIsValid(PortalState state) noexcept {
  switch (state) {
    case PortalState::declared:
    case PortalState::open:
    case PortalState::fetching:
    case PortalState::exhausted:
    case PortalState::detached:
    case PortalState::closed:
    case PortalState::cleanup_pending:
    case PortalState::error:
      return true;
  }
  return false;
}

struct ExecutionCursorHandle {
  Uuid cursor_uuid{};
  ExecutionRelationDescriptor relation_descriptor;
  PortalState state = PortalState::declared;
  Uuid owner_session_uuid{};
  Uuid owner_transaction_uuid{};
  Uuid snapshot_uuid{};
  Uuid security_policy_uuid{};
  bool close_owner = true;
  bool transferable = false;
  bool backpressure_enabled = true;
  std::uint32_t stream_window_rows = 1;
  std::uint64_t lifetime_epoch = 1;
  bool cleanup_required = false;
};

enum class ExecutionCursorHandleStatus : std::uint8_t {
  ok = 0,
  cursor_uuid_required = 1,
  relation_descriptor_invalid = 2,
  relation_descriptor_kind_invalid = 3,
  owner_session_uuid_required = 4,
  owner_transaction_uuid_required = 5,
  snapshot_uuid_required = 6,
  security_policy_uuid_required = 7,
  lifetime_epoch_required = 8,
  portal_state_invalid = 9,
  backpressure_window_required = 10,
  cleanup_owner_required = 11,
  closed_handle_has_open_window = 12
};

constexpr std::string_view ExecutionCursorHandleStatusName(
    ExecutionCursorHandleStatus status) noexcept {
  switch (status) {
    case ExecutionCursorHandleStatus::ok:
      return "ok";
    case ExecutionCursorHandleStatus::cursor_uuid_required:
      return "cursor_uuid_required";
    case ExecutionCursorHandleStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionCursorHandleStatus::relation_descriptor_kind_invalid:
      return "relation_descriptor_kind_invalid";
    case ExecutionCursorHandleStatus::owner_session_uuid_required:
      return "owner_session_uuid_required";
    case ExecutionCursorHandleStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionCursorHandleStatus::snapshot_uuid_required:
      return "snapshot_uuid_required";
    case ExecutionCursorHandleStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionCursorHandleStatus::lifetime_epoch_required:
      return "lifetime_epoch_required";
    case ExecutionCursorHandleStatus::portal_state_invalid:
      return "portal_state_invalid";
    case ExecutionCursorHandleStatus::backpressure_window_required:
      return "backpressure_window_required";
    case ExecutionCursorHandleStatus::cleanup_owner_required:
      return "cleanup_owner_required";
    case ExecutionCursorHandleStatus::closed_handle_has_open_window:
      return "closed_handle_has_open_window";
  }
  return "unknown_status";
}

struct ExecutionCursorHandleValidationResult {
  ExecutionCursorHandleStatus status = ExecutionCursorHandleStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionCursorHandleStatus::ok;
  }
};

inline ExecutionCursorHandleValidationResult ValidateExecutionCursorHandle(
    const ExecutionCursorHandle& handle) {
  if (ExecutionDataPacketUuidIsNil(handle.cursor_uuid)) {
    return {ExecutionCursorHandleStatus::cursor_uuid_required};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(handle.relation_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionCursorHandleStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (handle.relation_descriptor.relation_kind !=
      ExecutionRelationKind::cursor) {
    return {ExecutionCursorHandleStatus::relation_descriptor_kind_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(handle.owner_session_uuid)) {
    return {ExecutionCursorHandleStatus::owner_session_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(handle.owner_transaction_uuid)) {
    return {ExecutionCursorHandleStatus::owner_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(handle.snapshot_uuid)) {
    return {ExecutionCursorHandleStatus::snapshot_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(handle.security_policy_uuid)) {
    return {ExecutionCursorHandleStatus::security_policy_uuid_required};
  }
  if (handle.lifetime_epoch == 0) {
    return {ExecutionCursorHandleStatus::lifetime_epoch_required};
  }
  if (!PortalStateIsValid(handle.state)) {
    return {ExecutionCursorHandleStatus::portal_state_invalid};
  }
  if (handle.backpressure_enabled && handle.stream_window_rows == 0 &&
      handle.state != PortalState::closed) {
    return {ExecutionCursorHandleStatus::backpressure_window_required};
  }
  if (handle.state == PortalState::cleanup_pending && !handle.close_owner) {
    return {ExecutionCursorHandleStatus::cleanup_owner_required};
  }
  if (handle.state == PortalState::closed && handle.stream_window_rows != 0) {
    return {ExecutionCursorHandleStatus::closed_handle_has_open_window};
  }
  return {};
}

inline bool ExecutionTypeDescriptorIdentityEquals(
    const ExecutionTypeDescriptor& left,
    const ExecutionTypeDescriptor& right) noexcept;

// SEARCH_KEY: EDR_EXECUTION_ROWSET_VALUE
enum class ExecutionRowsetStorageKind : std::uint8_t {
  inline_rows = 0,
  spill_handle = 1
};

constexpr bool ExecutionRowsetStorageKindIsValid(
    ExecutionRowsetStorageKind storage_kind) noexcept {
  switch (storage_kind) {
    case ExecutionRowsetStorageKind::inline_rows:
    case ExecutionRowsetStorageKind::spill_handle:
      return true;
  }
  return false;
}

struct ExecutionRowsetValue {
  Uuid rowset_uuid{};
  ExecutionRelationDescriptor relation_descriptor;
  ExecutionRowsetStorageKind storage_kind =
      ExecutionRowsetStorageKind::inline_rows;
  std::vector<ExecutionRowBatch> row_batches;
  Uuid spill_handle_uuid{};
  Uuid owner_transaction_uuid{};
  Uuid memory_owner_uuid{};
  bool bounded = true;
  std::uint32_t max_rows = kExecutionRowBatchHardMaxRows;
  std::uint64_t memory_bytes = 0;
  std::uint64_t memory_limit_bytes = 1;
  bool copyable = true;
  bool movable = true;
};

enum class ExecutionRowsetValueStatus : std::uint8_t {
  ok = 0,
  rowset_uuid_required = 1,
  relation_descriptor_invalid = 2,
  relation_descriptor_kind_invalid = 3,
  owner_transaction_uuid_required = 4,
  memory_owner_uuid_required = 5,
  unbounded_rowset = 6,
  max_rows_required = 7,
  row_count_exceeds_max = 8,
  memory_limit_required = 9,
  memory_limit_exceeded = 10,
  storage_kind_invalid = 11,
  inline_rows_required = 12,
  inline_batch_invalid = 13,
  inline_batch_shape_mismatch = 14,
  spill_handle_uuid_required = 15,
  spill_inline_rows_forbidden = 16,
  not_copyable = 17,
  not_movable = 18
};

constexpr std::string_view ExecutionRowsetValueStatusName(
    ExecutionRowsetValueStatus status) noexcept {
  switch (status) {
    case ExecutionRowsetValueStatus::ok:
      return "ok";
    case ExecutionRowsetValueStatus::rowset_uuid_required:
      return "rowset_uuid_required";
    case ExecutionRowsetValueStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionRowsetValueStatus::relation_descriptor_kind_invalid:
      return "relation_descriptor_kind_invalid";
    case ExecutionRowsetValueStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionRowsetValueStatus::memory_owner_uuid_required:
      return "memory_owner_uuid_required";
    case ExecutionRowsetValueStatus::unbounded_rowset:
      return "unbounded_rowset";
    case ExecutionRowsetValueStatus::max_rows_required:
      return "max_rows_required";
    case ExecutionRowsetValueStatus::row_count_exceeds_max:
      return "row_count_exceeds_max";
    case ExecutionRowsetValueStatus::memory_limit_required:
      return "memory_limit_required";
    case ExecutionRowsetValueStatus::memory_limit_exceeded:
      return "memory_limit_exceeded";
    case ExecutionRowsetValueStatus::storage_kind_invalid:
      return "storage_kind_invalid";
    case ExecutionRowsetValueStatus::inline_rows_required:
      return "inline_rows_required";
    case ExecutionRowsetValueStatus::inline_batch_invalid:
      return "inline_batch_invalid";
    case ExecutionRowsetValueStatus::inline_batch_shape_mismatch:
      return "inline_batch_shape_mismatch";
    case ExecutionRowsetValueStatus::spill_handle_uuid_required:
      return "spill_handle_uuid_required";
    case ExecutionRowsetValueStatus::spill_inline_rows_forbidden:
      return "spill_inline_rows_forbidden";
    case ExecutionRowsetValueStatus::not_copyable:
      return "not_copyable";
    case ExecutionRowsetValueStatus::not_movable:
      return "not_movable";
  }
  return "unknown_status";
}

struct ExecutionRowsetValueValidationResult {
  ExecutionRowsetValueStatus status = ExecutionRowsetValueStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionRowBatchStatus row_batch_status = ExecutionRowBatchStatus::ok;
  ExecutionDataPacketStatus packet_status = ExecutionDataPacketStatus::ok;
  std::size_t batch_index = 0;
  std::size_t column_index = 0;

  bool ok() const noexcept {
    return status == ExecutionRowsetValueStatus::ok;
  }
};

inline ExecutionRowsetValueValidationResult ValidateExecutionRowsetValue(
    const ExecutionRowsetValue& value) {
  if (ExecutionDataPacketUuidIsNil(value.rowset_uuid)) {
    return {ExecutionRowsetValueStatus::rowset_uuid_required};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(value.relation_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionRowsetValueStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (value.relation_descriptor.relation_kind !=
      ExecutionRelationKind::rowset) {
    return {ExecutionRowsetValueStatus::relation_descriptor_kind_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(value.owner_transaction_uuid)) {
    return {ExecutionRowsetValueStatus::owner_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(value.memory_owner_uuid)) {
    return {ExecutionRowsetValueStatus::memory_owner_uuid_required};
  }
  if (!value.bounded) {
    return {ExecutionRowsetValueStatus::unbounded_rowset};
  }
  if (value.max_rows == 0) {
    return {ExecutionRowsetValueStatus::max_rows_required};
  }
  if (value.memory_limit_bytes == 0) {
    return {ExecutionRowsetValueStatus::memory_limit_required};
  }
  if (value.memory_bytes > value.memory_limit_bytes) {
    return {ExecutionRowsetValueStatus::memory_limit_exceeded};
  }
  if (!ExecutionRowsetStorageKindIsValid(value.storage_kind)) {
    return {ExecutionRowsetValueStatus::storage_kind_invalid};
  }
  if (!value.copyable) {
    return {ExecutionRowsetValueStatus::not_copyable};
  }
  if (!value.movable) {
    return {ExecutionRowsetValueStatus::not_movable};
  }

  if (value.storage_kind == ExecutionRowsetStorageKind::spill_handle) {
    if (ExecutionDataPacketUuidIsNil(value.spill_handle_uuid)) {
      return {ExecutionRowsetValueStatus::spill_handle_uuid_required};
    }
    if (!value.row_batches.empty()) {
      return {ExecutionRowsetValueStatus::spill_inline_rows_forbidden};
    }
    return {};
  }

  if (value.row_batches.empty()) {
    return {ExecutionRowsetValueStatus::inline_rows_required};
  }

  std::uint64_t observed_rows = 0;
  for (std::size_t batch_index = 0; batch_index < value.row_batches.size();
       ++batch_index) {
    const auto& batch = value.row_batches[batch_index];
    const auto batch_result = ValidateExecutionRowBatch(batch);
    if (!batch_result.ok()) {
      return {ExecutionRowsetValueStatus::inline_batch_invalid,
              ExecutionRelationDescriptorStatus::ok, batch_result.status,
              batch_result.packet_status, batch_index,
              batch_result.column_index};
    }
    if (batch.row_shape.columns.size() !=
        value.relation_descriptor.columns.size()) {
      return {ExecutionRowsetValueStatus::inline_batch_shape_mismatch,
              ExecutionRelationDescriptorStatus::ok, ExecutionRowBatchStatus::ok,
              ExecutionDataPacketStatus::ok, batch_index, 0};
    }
    for (std::size_t column_index = 0;
         column_index < batch.row_shape.columns.size(); ++column_index) {
      const auto descriptor_index =
          batch.row_shape.columns[column_index].descriptor_index;
      const auto& batch_descriptor =
          batch.data_packet.descriptor_table[descriptor_index];
      if (!ExecutionTypeDescriptorIdentityEquals(
              value.relation_descriptor.columns[column_index].descriptor,
              batch_descriptor) ||
          batch.row_shape.columns[column_index].nullable !=
              value.relation_descriptor.columns[column_index].nullable) {
        return {ExecutionRowsetValueStatus::inline_batch_shape_mismatch,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionRowBatchStatus::ok, ExecutionDataPacketStatus::ok,
                batch_index, column_index};
      }
    }
    observed_rows += batch.rows.size();
    if (observed_rows > value.max_rows) {
      return {ExecutionRowsetValueStatus::row_count_exceeds_max,
              ExecutionRelationDescriptorStatus::ok, ExecutionRowBatchStatus::ok,
              ExecutionDataPacketStatus::ok, batch_index, 0};
    }
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_TABLE_VALUE
enum class ExecutionTableValueProducerKind : std::uint8_t {
  sblr_operator = 0,
  routine = 1,
  udr = 2,
  result_channel = 3
};

constexpr bool ExecutionTableValueProducerKindIsValid(
    ExecutionTableValueProducerKind producer_kind) noexcept {
  switch (producer_kind) {
    case ExecutionTableValueProducerKind::sblr_operator:
    case ExecutionTableValueProducerKind::routine:
    case ExecutionTableValueProducerKind::udr:
    case ExecutionTableValueProducerKind::result_channel:
      return true;
  }
  return false;
}

struct ExecutionTableValue {
  Uuid table_value_uuid{};
  ExecutionRelationDescriptor relation_descriptor;
  ExecutionTableValueProducerKind producer_kind =
      ExecutionTableValueProducerKind::sblr_operator;
  Uuid producer_uuid{};
  Uuid owner_transaction_uuid{};
  Uuid snapshot_uuid{};
  Uuid security_policy_uuid{};
  bool consumable_by_sblr_operator = true;
  bool consumable_by_routine = true;
  bool consumable_by_udr = true;
  bool consumable_by_result_channel = true;
  bool rewindable = true;
  bool deterministic = true;
  bool side_effecting = false;
  bool plan_requires_rewind = false;
  bool plan_may_duplicate = false;
};

enum class ExecutionTableValueStatus : std::uint8_t {
  ok = 0,
  table_value_uuid_required = 1,
  relation_descriptor_invalid = 2,
  relation_descriptor_kind_invalid = 3,
  producer_kind_invalid = 4,
  producer_uuid_required = 5,
  owner_transaction_uuid_required = 6,
  snapshot_uuid_required = 7,
  security_policy_uuid_required = 8,
  consumer_surface_required = 9,
  non_rewindable_plan_refused = 10,
  nondeterministic_rewind_refused = 11,
  side_effecting_duplicate_refused = 12
};

constexpr std::string_view ExecutionTableValueStatusName(
    ExecutionTableValueStatus status) noexcept {
  switch (status) {
    case ExecutionTableValueStatus::ok:
      return "ok";
    case ExecutionTableValueStatus::table_value_uuid_required:
      return "table_value_uuid_required";
    case ExecutionTableValueStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionTableValueStatus::relation_descriptor_kind_invalid:
      return "relation_descriptor_kind_invalid";
    case ExecutionTableValueStatus::producer_kind_invalid:
      return "producer_kind_invalid";
    case ExecutionTableValueStatus::producer_uuid_required:
      return "producer_uuid_required";
    case ExecutionTableValueStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionTableValueStatus::snapshot_uuid_required:
      return "snapshot_uuid_required";
    case ExecutionTableValueStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionTableValueStatus::consumer_surface_required:
      return "consumer_surface_required";
    case ExecutionTableValueStatus::non_rewindable_plan_refused:
      return "non_rewindable_plan_refused";
    case ExecutionTableValueStatus::nondeterministic_rewind_refused:
      return "nondeterministic_rewind_refused";
    case ExecutionTableValueStatus::side_effecting_duplicate_refused:
      return "side_effecting_duplicate_refused";
  }
  return "unknown_status";
}

struct ExecutionTableValueValidationResult {
  ExecutionTableValueStatus status = ExecutionTableValueStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionTableValueStatus::ok;
  }
};

inline ExecutionTableValueValidationResult ValidateExecutionTableValue(
    const ExecutionTableValue& value) {
  if (ExecutionDataPacketUuidIsNil(value.table_value_uuid)) {
    return {ExecutionTableValueStatus::table_value_uuid_required};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(value.relation_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionTableValueStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (value.relation_descriptor.relation_kind !=
      ExecutionRelationKind::table_value) {
    return {ExecutionTableValueStatus::relation_descriptor_kind_invalid};
  }
  if (!ExecutionTableValueProducerKindIsValid(value.producer_kind)) {
    return {ExecutionTableValueStatus::producer_kind_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(value.producer_uuid)) {
    return {ExecutionTableValueStatus::producer_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(value.owner_transaction_uuid)) {
    return {ExecutionTableValueStatus::owner_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(value.snapshot_uuid)) {
    return {ExecutionTableValueStatus::snapshot_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(value.security_policy_uuid)) {
    return {ExecutionTableValueStatus::security_policy_uuid_required};
  }
  if (!value.consumable_by_sblr_operator && !value.consumable_by_routine &&
      !value.consumable_by_udr && !value.consumable_by_result_channel) {
    return {ExecutionTableValueStatus::consumer_surface_required};
  }
  if (value.plan_requires_rewind && !value.rewindable) {
    return {ExecutionTableValueStatus::non_rewindable_plan_refused};
  }
  if (value.plan_requires_rewind && !value.deterministic) {
    return {ExecutionTableValueStatus::nondeterministic_rewind_refused};
  }
  if (value.plan_may_duplicate && value.side_effecting) {
    return {ExecutionTableValueStatus::side_effecting_duplicate_refused};
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_ROUTINE_SIGNATURE_DESCRIPTOR
enum class ExecutionRoutineKind : std::uint8_t {
  procedure = 0,
  function = 1,
  trigger_routine = 2,
  internal_procedure = 3
};

constexpr bool ExecutionRoutineKindIsValid(
    ExecutionRoutineKind routine_kind) noexcept {
  switch (routine_kind) {
    case ExecutionRoutineKind::procedure:
    case ExecutionRoutineKind::function:
    case ExecutionRoutineKind::trigger_routine:
    case ExecutionRoutineKind::internal_procedure:
      return true;
  }
  return false;
}

enum class ExecutionRoutineParameterDirection : std::uint8_t {
  in = 0,
  out = 1,
  inout = 2
};

constexpr bool ExecutionRoutineParameterDirectionIsValid(
    ExecutionRoutineParameterDirection direction) noexcept {
  switch (direction) {
    case ExecutionRoutineParameterDirection::in:
    case ExecutionRoutineParameterDirection::out:
    case ExecutionRoutineParameterDirection::inout:
      return true;
  }
  return false;
}

enum class ExecutionRoutineParameterKind : std::uint8_t {
  scalar = 0,
  cursor = 1,
  rowset = 2,
  table_value = 3
};

constexpr bool ExecutionRoutineParameterKindIsValid(
    ExecutionRoutineParameterKind parameter_kind) noexcept {
  switch (parameter_kind) {
    case ExecutionRoutineParameterKind::scalar:
    case ExecutionRoutineParameterKind::cursor:
    case ExecutionRoutineParameterKind::rowset:
    case ExecutionRoutineParameterKind::table_value:
      return true;
  }
  return false;
}

struct ExecutionRoutineParameterDescriptor {
  std::uint32_t ordinal = 0;
  std::string stable_name;
  ExecutionRoutineParameterDirection direction =
      ExecutionRoutineParameterDirection::in;
  ExecutionRoutineParameterKind parameter_kind =
      ExecutionRoutineParameterKind::scalar;
  ExecutionTypeDescriptor scalar_descriptor;
  ExecutionRelationDescriptor relation_descriptor;
  bool has_default = false;
  bool default_descriptor_authoritative = true;
  bool named_binding_allowed = true;
  bool nullable = true;
};

struct ExecutionRoutineResultDescriptor {
  std::uint32_t ordinal = 0;
  std::string stable_name;
  ExecutionRelationDescriptor relation_descriptor;
  bool default_result = false;
};

struct ExecutionRoutineSignatureDescriptor {
  Uuid routine_signature_uuid{};
  std::uint64_t signature_epoch = 0;
  Uuid routine_uuid{};
  ExecutionRoutineKind routine_kind = ExecutionRoutineKind::procedure;
  std::string stable_name;
  std::vector<ExecutionRoutineParameterDescriptor> parameter_descriptors;
  std::vector<ExecutionRoutineResultDescriptor> result_descriptors;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionRoutineSignatureDescriptorStatus : std::uint8_t {
  ok = 0,
  signature_uuid_required = 1,
  signature_epoch_required = 2,
  routine_uuid_required = 3,
  stable_name_required = 4,
  descriptor_not_authoritative = 5,
  descriptor_parser_dependent = 6,
  routine_kind_invalid = 7,
  parameter_ordinal_mismatch = 8,
  parameter_name_required = 9,
  parameter_direction_invalid = 10,
  parameter_kind_invalid = 11,
  scalar_parameter_descriptor_invalid = 12,
  relation_parameter_descriptor_invalid = 13,
  relation_parameter_kind_mismatch = 14,
  default_descriptor_required = 15,
  out_parameter_default_forbidden = 16,
  result_ordinal_mismatch = 17,
  result_name_required = 18,
  result_relation_descriptor_invalid = 19,
  result_relation_kind_invalid = 20,
  function_result_required = 21
};

constexpr std::string_view ExecutionRoutineSignatureDescriptorStatusName(
    ExecutionRoutineSignatureDescriptorStatus status) noexcept {
  switch (status) {
    case ExecutionRoutineSignatureDescriptorStatus::ok:
      return "ok";
    case ExecutionRoutineSignatureDescriptorStatus::signature_uuid_required:
      return "signature_uuid_required";
    case ExecutionRoutineSignatureDescriptorStatus::signature_epoch_required:
      return "signature_epoch_required";
    case ExecutionRoutineSignatureDescriptorStatus::routine_uuid_required:
      return "routine_uuid_required";
    case ExecutionRoutineSignatureDescriptorStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionRoutineSignatureDescriptorStatus::
        descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionRoutineSignatureDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionRoutineSignatureDescriptorStatus::routine_kind_invalid:
      return "routine_kind_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::parameter_ordinal_mismatch:
      return "parameter_ordinal_mismatch";
    case ExecutionRoutineSignatureDescriptorStatus::parameter_name_required:
      return "parameter_name_required";
    case ExecutionRoutineSignatureDescriptorStatus::parameter_direction_invalid:
      return "parameter_direction_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::parameter_kind_invalid:
      return "parameter_kind_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::
        scalar_parameter_descriptor_invalid:
      return "scalar_parameter_descriptor_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::
        relation_parameter_descriptor_invalid:
      return "relation_parameter_descriptor_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::
        relation_parameter_kind_mismatch:
      return "relation_parameter_kind_mismatch";
    case ExecutionRoutineSignatureDescriptorStatus::default_descriptor_required:
      return "default_descriptor_required";
    case ExecutionRoutineSignatureDescriptorStatus::
        out_parameter_default_forbidden:
      return "out_parameter_default_forbidden";
    case ExecutionRoutineSignatureDescriptorStatus::result_ordinal_mismatch:
      return "result_ordinal_mismatch";
    case ExecutionRoutineSignatureDescriptorStatus::result_name_required:
      return "result_name_required";
    case ExecutionRoutineSignatureDescriptorStatus::
        result_relation_descriptor_invalid:
      return "result_relation_descriptor_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::
        result_relation_kind_invalid:
      return "result_relation_kind_invalid";
    case ExecutionRoutineSignatureDescriptorStatus::function_result_required:
      return "function_result_required";
  }
  return "unknown_status";
}

struct ExecutionRoutineSignatureDescriptorValidationResult {
  ExecutionRoutineSignatureDescriptorStatus status =
      ExecutionRoutineSignatureDescriptorStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionDataPacketStatus scalar_descriptor_status =
      ExecutionDataPacketStatus::ok;
  std::size_t parameter_index = 0;
  std::size_t result_index = 0;

  bool ok() const noexcept {
    return status == ExecutionRoutineSignatureDescriptorStatus::ok;
  }
};

inline ExecutionRoutineSignatureDescriptorValidationResult
ValidateExecutionRoutineSignatureDescriptor(
    const ExecutionRoutineSignatureDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.routine_signature_uuid)) {
    return {ExecutionRoutineSignatureDescriptorStatus::signature_uuid_required};
  }
  if (descriptor.signature_epoch == 0) {
    return {ExecutionRoutineSignatureDescriptorStatus::signature_epoch_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.routine_uuid)) {
    return {ExecutionRoutineSignatureDescriptorStatus::routine_uuid_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ExecutionRoutineSignatureDescriptorStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {
        ExecutionRoutineSignatureDescriptorStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {
        ExecutionRoutineSignatureDescriptorStatus::descriptor_parser_dependent};
  }
  if (!ExecutionRoutineKindIsValid(descriptor.routine_kind)) {
    return {ExecutionRoutineSignatureDescriptorStatus::routine_kind_invalid};
  }

  for (std::size_t parameter_index = 0;
       parameter_index < descriptor.parameter_descriptors.size();
       ++parameter_index) {
    const auto& parameter = descriptor.parameter_descriptors[parameter_index];
    if (parameter.ordinal != parameter_index) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::parameter_ordinal_mismatch,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }
    if (parameter.stable_name.empty()) {
      return {ExecutionRoutineSignatureDescriptorStatus::parameter_name_required,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, parameter_index, 0};
    }
    if (!ExecutionRoutineParameterDirectionIsValid(parameter.direction)) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::parameter_direction_invalid,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }
    if (!ExecutionRoutineParameterKindIsValid(parameter.parameter_kind)) {
      return {ExecutionRoutineSignatureDescriptorStatus::parameter_kind_invalid,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, parameter_index, 0};
    }
    if (parameter.has_default &&
        parameter.direction == ExecutionRoutineParameterDirection::out) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::
              out_parameter_default_forbidden,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }
    if (parameter.has_default && !parameter.default_descriptor_authoritative) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::default_descriptor_required,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }

    if (parameter.parameter_kind == ExecutionRoutineParameterKind::scalar) {
      const auto scalar_result = ValidateExecutionDataPacketDescriptor(
          parameter.scalar_descriptor, parameter_index);
      if (!scalar_result.ok()) {
        return {
            ExecutionRoutineSignatureDescriptorStatus::
                scalar_parameter_descriptor_invalid,
            ExecutionRelationDescriptorStatus::ok, scalar_result.status,
            parameter_index, 0};
      }
      continue;
    }

    const auto relation_result =
        ValidateExecutionRelationDescriptor(parameter.relation_descriptor);
    if (!relation_result.ok()) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::
              relation_parameter_descriptor_invalid,
          relation_result.status, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }

    if ((parameter.parameter_kind == ExecutionRoutineParameterKind::cursor &&
         parameter.relation_descriptor.relation_kind !=
             ExecutionRelationKind::cursor) ||
        (parameter.parameter_kind == ExecutionRoutineParameterKind::rowset &&
         parameter.relation_descriptor.relation_kind !=
             ExecutionRelationKind::rowset) ||
        (parameter.parameter_kind ==
             ExecutionRoutineParameterKind::table_value &&
         parameter.relation_descriptor.relation_kind !=
             ExecutionRelationKind::table_value)) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::
              relation_parameter_kind_mismatch,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          parameter_index, 0};
    }
  }

  for (std::size_t result_index = 0;
       result_index < descriptor.result_descriptors.size(); ++result_index) {
    const auto& result = descriptor.result_descriptors[result_index];
    if (result.ordinal != result_index) {
      return {ExecutionRoutineSignatureDescriptorStatus::result_ordinal_mismatch,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, 0, result_index};
    }
    if (result.stable_name.empty()) {
      return {ExecutionRoutineSignatureDescriptorStatus::result_name_required,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, 0, result_index};
    }
    const auto relation_result =
        ValidateExecutionRelationDescriptor(result.relation_descriptor);
    if (!relation_result.ok()) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::
              result_relation_descriptor_invalid,
          relation_result.status, ExecutionDataPacketStatus::ok, 0,
          result_index};
    }
    if (result.relation_descriptor.relation_kind !=
        ExecutionRelationKind::result_channel) {
      return {
          ExecutionRoutineSignatureDescriptorStatus::
              result_relation_kind_invalid,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          0, result_index};
    }
  }

  if (descriptor.routine_kind == ExecutionRoutineKind::function &&
      descriptor.result_descriptors.empty()) {
    return {ExecutionRoutineSignatureDescriptorStatus::function_result_required};
  }

  return {};
}

inline bool ExecutionRelationDescriptorIdentityEquals(
    const ExecutionRelationDescriptor& left,
    const ExecutionRelationDescriptor& right) noexcept {
  return ExecutionDataPacketUuidEquals(left.relation_descriptor_uuid,
                                       right.relation_descriptor_uuid) &&
         left.descriptor_epoch == right.descriptor_epoch &&
         left.relation_kind == right.relation_kind;
}

struct ExecutionRoutineCallArgumentDescriptor {
  std::uint32_t ordinal = 0;
  std::string stable_name;
  ExecutionRoutineParameterKind argument_kind =
      ExecutionRoutineParameterKind::scalar;
  bool supplied = true;
  bool default_requested = false;
  ExecutionTypeDescriptor scalar_descriptor;
  ExecutionRelationDescriptor relation_descriptor;
};

enum class ExecutionRoutineCallValidationStatus : std::uint8_t {
  ok = 0,
  signature_descriptor_invalid = 1,
  argument_count_mismatch = 2,
  argument_ordinal_mismatch = 3,
  argument_name_mismatch = 4,
  named_argument_not_allowed = 5,
  missing_required_argument = 6,
  default_argument_not_allowed = 7,
  argument_kind_mismatch = 8,
  scalar_argument_descriptor_invalid = 9,
  scalar_argument_descriptor_mismatch = 10,
  relation_argument_descriptor_invalid = 11,
  relation_argument_kind_mismatch = 12,
  relation_argument_descriptor_mismatch = 13,
  result_count_mismatch = 14,
  result_ordinal_mismatch = 15,
  result_name_mismatch = 16,
  result_relation_descriptor_invalid = 17,
  result_descriptor_mismatch = 18
};

constexpr std::string_view ExecutionRoutineCallValidationStatusName(
    ExecutionRoutineCallValidationStatus status) noexcept {
  switch (status) {
    case ExecutionRoutineCallValidationStatus::ok:
      return "ok";
    case ExecutionRoutineCallValidationStatus::signature_descriptor_invalid:
      return "signature_descriptor_invalid";
    case ExecutionRoutineCallValidationStatus::argument_count_mismatch:
      return "argument_count_mismatch";
    case ExecutionRoutineCallValidationStatus::argument_ordinal_mismatch:
      return "argument_ordinal_mismatch";
    case ExecutionRoutineCallValidationStatus::argument_name_mismatch:
      return "argument_name_mismatch";
    case ExecutionRoutineCallValidationStatus::named_argument_not_allowed:
      return "named_argument_not_allowed";
    case ExecutionRoutineCallValidationStatus::missing_required_argument:
      return "missing_required_argument";
    case ExecutionRoutineCallValidationStatus::default_argument_not_allowed:
      return "default_argument_not_allowed";
    case ExecutionRoutineCallValidationStatus::argument_kind_mismatch:
      return "argument_kind_mismatch";
    case ExecutionRoutineCallValidationStatus::scalar_argument_descriptor_invalid:
      return "scalar_argument_descriptor_invalid";
    case ExecutionRoutineCallValidationStatus::scalar_argument_descriptor_mismatch:
      return "scalar_argument_descriptor_mismatch";
    case ExecutionRoutineCallValidationStatus::
        relation_argument_descriptor_invalid:
      return "relation_argument_descriptor_invalid";
    case ExecutionRoutineCallValidationStatus::relation_argument_kind_mismatch:
      return "relation_argument_kind_mismatch";
    case ExecutionRoutineCallValidationStatus::
        relation_argument_descriptor_mismatch:
      return "relation_argument_descriptor_mismatch";
    case ExecutionRoutineCallValidationStatus::result_count_mismatch:
      return "result_count_mismatch";
    case ExecutionRoutineCallValidationStatus::result_ordinal_mismatch:
      return "result_ordinal_mismatch";
    case ExecutionRoutineCallValidationStatus::result_name_mismatch:
      return "result_name_mismatch";
    case ExecutionRoutineCallValidationStatus::
        result_relation_descriptor_invalid:
      return "result_relation_descriptor_invalid";
    case ExecutionRoutineCallValidationStatus::result_descriptor_mismatch:
      return "result_descriptor_mismatch";
  }
  return "unknown_status";
}

struct ExecutionRoutineCallValidationResult {
  ExecutionRoutineCallValidationStatus status =
      ExecutionRoutineCallValidationStatus::ok;
  ExecutionRoutineSignatureDescriptorStatus signature_status =
      ExecutionRoutineSignatureDescriptorStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionDataPacketStatus scalar_descriptor_status =
      ExecutionDataPacketStatus::ok;
  std::size_t argument_index = 0;
  std::size_t result_index = 0;

  bool ok() const noexcept {
    return status == ExecutionRoutineCallValidationStatus::ok;
  }
};

inline ExecutionRoutineCallValidationResult
ValidateExecutionRoutineCallAgainstSignature(
    const ExecutionRoutineSignatureDescriptor& signature,
    std::span<const ExecutionRoutineCallArgumentDescriptor> arguments,
    std::span<const ExecutionRoutineResultDescriptor> result_descriptors) {
  const auto signature_result =
      ValidateExecutionRoutineSignatureDescriptor(signature);
  if (!signature_result.ok()) {
    return {ExecutionRoutineCallValidationStatus::signature_descriptor_invalid,
            signature_result.status};
  }

  if (arguments.size() != signature.parameter_descriptors.size()) {
    return {ExecutionRoutineCallValidationStatus::argument_count_mismatch};
  }

  for (std::size_t argument_index = 0; argument_index < arguments.size();
       ++argument_index) {
    const auto& argument = arguments[argument_index];
    const auto& parameter = signature.parameter_descriptors[argument_index];
    if (argument.ordinal != argument_index) {
      return {ExecutionRoutineCallValidationStatus::argument_ordinal_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, argument_index, 0};
    }
    if (!argument.stable_name.empty()) {
      if (!parameter.named_binding_allowed) {
        return {ExecutionRoutineCallValidationStatus::named_argument_not_allowed,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, argument_index, 0};
      }
      if (argument.stable_name != parameter.stable_name) {
        return {ExecutionRoutineCallValidationStatus::argument_name_mismatch,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, argument_index, 0};
      }
    }

    if (!argument.supplied) {
      if (parameter.direction == ExecutionRoutineParameterDirection::out ||
          parameter.has_default) {
        continue;
      }
      return {ExecutionRoutineCallValidationStatus::missing_required_argument,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, argument_index, 0};
    }

    if (argument.default_requested) {
      if (!parameter.has_default ||
          parameter.direction == ExecutionRoutineParameterDirection::out) {
        return {
            ExecutionRoutineCallValidationStatus::default_argument_not_allowed,
            ExecutionRoutineSignatureDescriptorStatus::ok,
            ExecutionRelationDescriptorStatus::ok,
            ExecutionDataPacketStatus::ok, argument_index, 0};
      }
      continue;
    }

    if (argument.argument_kind != parameter.parameter_kind) {
      return {ExecutionRoutineCallValidationStatus::argument_kind_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, argument_index, 0};
    }

    if (parameter.parameter_kind == ExecutionRoutineParameterKind::scalar) {
      const auto scalar_result = ValidateExecutionDataPacketDescriptor(
          argument.scalar_descriptor, argument_index);
      if (!scalar_result.ok()) {
        return {
            ExecutionRoutineCallValidationStatus::
                scalar_argument_descriptor_invalid,
            ExecutionRoutineSignatureDescriptorStatus::ok,
            ExecutionRelationDescriptorStatus::ok, scalar_result.status,
            argument_index, 0};
      }
      if (!ExecutionTypeDescriptorIdentityEquals(parameter.scalar_descriptor,
                                                argument.scalar_descriptor)) {
        return {
            ExecutionRoutineCallValidationStatus::
                scalar_argument_descriptor_mismatch,
            ExecutionRoutineSignatureDescriptorStatus::ok,
            ExecutionRelationDescriptorStatus::ok,
            ExecutionDataPacketStatus::ok, argument_index, 0};
      }
      continue;
    }

    const auto relation_result =
        ValidateExecutionRelationDescriptor(argument.relation_descriptor);
    if (!relation_result.ok()) {
      return {
          ExecutionRoutineCallValidationStatus::
              relation_argument_descriptor_invalid,
          ExecutionRoutineSignatureDescriptorStatus::ok, relation_result.status,
          ExecutionDataPacketStatus::ok, argument_index, 0};
    }
    if ((parameter.parameter_kind == ExecutionRoutineParameterKind::cursor &&
         argument.relation_descriptor.relation_kind !=
             ExecutionRelationKind::cursor) ||
        (parameter.parameter_kind == ExecutionRoutineParameterKind::rowset &&
         argument.relation_descriptor.relation_kind !=
             ExecutionRelationKind::rowset) ||
        (parameter.parameter_kind ==
             ExecutionRoutineParameterKind::table_value &&
         argument.relation_descriptor.relation_kind !=
             ExecutionRelationKind::table_value)) {
      return {
          ExecutionRoutineCallValidationStatus::relation_argument_kind_mismatch,
          ExecutionRoutineSignatureDescriptorStatus::ok,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          argument_index, 0};
    }
    if (!ExecutionRelationDescriptorIdentityEquals(
            parameter.relation_descriptor, argument.relation_descriptor)) {
      return {
          ExecutionRoutineCallValidationStatus::
              relation_argument_descriptor_mismatch,
          ExecutionRoutineSignatureDescriptorStatus::ok,
          ExecutionRelationDescriptorStatus::ok, ExecutionDataPacketStatus::ok,
          argument_index, 0};
    }
  }

  if (result_descriptors.size() != signature.result_descriptors.size()) {
    return {ExecutionRoutineCallValidationStatus::result_count_mismatch};
  }

  for (std::size_t result_index = 0; result_index < result_descriptors.size();
       ++result_index) {
    const auto& expected = signature.result_descriptors[result_index];
    const auto& actual = result_descriptors[result_index];
    if (actual.ordinal != result_index) {
      return {ExecutionRoutineCallValidationStatus::result_ordinal_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, 0, result_index};
    }
    if (actual.stable_name != expected.stable_name) {
      return {ExecutionRoutineCallValidationStatus::result_name_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, 0, result_index};
    }
    const auto relation_result =
        ValidateExecutionRelationDescriptor(actual.relation_descriptor);
    if (!relation_result.ok()) {
      return {
          ExecutionRoutineCallValidationStatus::
              result_relation_descriptor_invalid,
          ExecutionRoutineSignatureDescriptorStatus::ok, relation_result.status,
          ExecutionDataPacketStatus::ok, 0, result_index};
    }
    if (!ExecutionRelationDescriptorIdentityEquals(
            expected.relation_descriptor, actual.relation_descriptor)) {
      return {ExecutionRoutineCallValidationStatus::result_descriptor_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, 0, result_index};
    }
  }

  return {};
}

// SEARCH_KEY: polymorphic descriptor
// SEARCH_KEY: ANYELEMENT
// SEARCH_KEY: same domain stack
enum class PolymorphicRoutineTypeKind : std::uint8_t {
  concrete = 0,
  any = 1,
  anyelement = 2,
  anyrow = 3,
  anytable = 4,
  anycursor = 5
};

constexpr bool PolymorphicRoutineTypeKindIsValid(
    PolymorphicRoutineTypeKind kind) noexcept {
  switch (kind) {
    case PolymorphicRoutineTypeKind::concrete:
    case PolymorphicRoutineTypeKind::any:
    case PolymorphicRoutineTypeKind::anyelement:
    case PolymorphicRoutineTypeKind::anyrow:
    case PolymorphicRoutineTypeKind::anytable:
    case PolymorphicRoutineTypeKind::anycursor:
      return true;
  }
  return false;
}

enum class PolymorphicRoutineBindingMatchPolicy : std::uint8_t {
  same_type = 0,
  same_domain_stack = 1
};

constexpr bool PolymorphicRoutineBindingMatchPolicyIsValid(
    PolymorphicRoutineBindingMatchPolicy policy) noexcept {
  switch (policy) {
    case PolymorphicRoutineBindingMatchPolicy::same_type:
    case PolymorphicRoutineBindingMatchPolicy::same_domain_stack:
      return true;
  }
  return false;
}

struct PolymorphicRoutineBindingSlot {
  std::uint32_t ordinal = 0;
  std::string stable_name;
  ExecutionRoutineParameterKind parameter_kind =
      ExecutionRoutineParameterKind::scalar;
  PolymorphicRoutineTypeKind polymorphic_kind =
      PolymorphicRoutineTypeKind::concrete;
  PolymorphicRoutineBindingMatchPolicy match_policy =
      PolymorphicRoutineBindingMatchPolicy::same_type;
  Uuid binding_group_uuid{};
  ExecutionTypeDescriptor scalar_descriptor;
  ExecutionRelationDescriptor relation_descriptor;
};

struct PolymorphicRoutineBindingDescriptor {
  Uuid binding_uuid{};
  Uuid routine_signature_uuid{};
  std::uint64_t binding_epoch = 0;
  std::uint64_t signature_epoch = 0;
  std::string stable_name;
  std::vector<PolymorphicRoutineBindingSlot> slots;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class PolymorphicRoutineBindingStatus : std::uint8_t {
  ok = 0,
  binding_uuid_required = 1,
  routine_signature_uuid_required = 2,
  binding_epoch_required = 3,
  signature_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  signature_descriptor_invalid = 8,
  signature_uuid_mismatch = 9,
  signature_epoch_mismatch = 10,
  slot_count_mismatch = 11,
  slot_ordinal_mismatch = 12,
  slot_name_mismatch = 13,
  slot_parameter_kind_mismatch = 14,
  polymorphic_kind_invalid = 15,
  binding_policy_invalid = 16,
  polymorphic_kind_incompatible = 17,
  binding_group_uuid_required = 18,
  scalar_descriptor_invalid = 19,
  relation_descriptor_invalid = 20,
  relation_kind_mismatch = 21,
  concrete_scalar_descriptor_mismatch = 22,
  concrete_relation_descriptor_mismatch = 23,
  binding_group_kind_mismatch = 24,
  binding_group_policy_mismatch = 25,
  binding_group_descriptor_mismatch = 26,
  binding_group_domain_stack_mismatch = 27
};

constexpr std::string_view PolymorphicRoutineBindingStatusName(
    PolymorphicRoutineBindingStatus status) noexcept {
  switch (status) {
    case PolymorphicRoutineBindingStatus::ok:
      return "ok";
    case PolymorphicRoutineBindingStatus::binding_uuid_required:
      return "binding_uuid_required";
    case PolymorphicRoutineBindingStatus::routine_signature_uuid_required:
      return "routine_signature_uuid_required";
    case PolymorphicRoutineBindingStatus::binding_epoch_required:
      return "binding_epoch_required";
    case PolymorphicRoutineBindingStatus::signature_epoch_required:
      return "signature_epoch_required";
    case PolymorphicRoutineBindingStatus::stable_name_required:
      return "stable_name_required";
    case PolymorphicRoutineBindingStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case PolymorphicRoutineBindingStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case PolymorphicRoutineBindingStatus::signature_descriptor_invalid:
      return "signature_descriptor_invalid";
    case PolymorphicRoutineBindingStatus::signature_uuid_mismatch:
      return "signature_uuid_mismatch";
    case PolymorphicRoutineBindingStatus::signature_epoch_mismatch:
      return "signature_epoch_mismatch";
    case PolymorphicRoutineBindingStatus::slot_count_mismatch:
      return "slot_count_mismatch";
    case PolymorphicRoutineBindingStatus::slot_ordinal_mismatch:
      return "slot_ordinal_mismatch";
    case PolymorphicRoutineBindingStatus::slot_name_mismatch:
      return "slot_name_mismatch";
    case PolymorphicRoutineBindingStatus::slot_parameter_kind_mismatch:
      return "slot_parameter_kind_mismatch";
    case PolymorphicRoutineBindingStatus::polymorphic_kind_invalid:
      return "polymorphic_kind_invalid";
    case PolymorphicRoutineBindingStatus::binding_policy_invalid:
      return "binding_policy_invalid";
    case PolymorphicRoutineBindingStatus::polymorphic_kind_incompatible:
      return "polymorphic_kind_incompatible";
    case PolymorphicRoutineBindingStatus::binding_group_uuid_required:
      return "binding_group_uuid_required";
    case PolymorphicRoutineBindingStatus::scalar_descriptor_invalid:
      return "scalar_descriptor_invalid";
    case PolymorphicRoutineBindingStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case PolymorphicRoutineBindingStatus::relation_kind_mismatch:
      return "relation_kind_mismatch";
    case PolymorphicRoutineBindingStatus::concrete_scalar_descriptor_mismatch:
      return "concrete_scalar_descriptor_mismatch";
    case PolymorphicRoutineBindingStatus::concrete_relation_descriptor_mismatch:
      return "concrete_relation_descriptor_mismatch";
    case PolymorphicRoutineBindingStatus::binding_group_kind_mismatch:
      return "binding_group_kind_mismatch";
    case PolymorphicRoutineBindingStatus::binding_group_policy_mismatch:
      return "binding_group_policy_mismatch";
    case PolymorphicRoutineBindingStatus::binding_group_descriptor_mismatch:
      return "binding_group_descriptor_mismatch";
    case PolymorphicRoutineBindingStatus::binding_group_domain_stack_mismatch:
      return "binding_group_domain_stack_mismatch";
  }
  return "unknown_status";
}

struct PolymorphicRoutineBindingValidationResult {
  PolymorphicRoutineBindingStatus status =
      PolymorphicRoutineBindingStatus::ok;
  ExecutionRoutineSignatureDescriptorStatus signature_status =
      ExecutionRoutineSignatureDescriptorStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionDataPacketStatus scalar_descriptor_status =
      ExecutionDataPacketStatus::ok;
  std::size_t slot_index = 0;

  bool ok() const noexcept {
    return status == PolymorphicRoutineBindingStatus::ok;
  }
};

inline bool PolymorphicRoutineTypeKindAcceptsParameter(
    PolymorphicRoutineTypeKind polymorphic_kind,
    ExecutionRoutineParameterKind parameter_kind) noexcept {
  switch (polymorphic_kind) {
    case PolymorphicRoutineTypeKind::concrete:
    case PolymorphicRoutineTypeKind::any:
      return true;
    case PolymorphicRoutineTypeKind::anyelement:
      return parameter_kind == ExecutionRoutineParameterKind::scalar;
    case PolymorphicRoutineTypeKind::anyrow:
      return parameter_kind == ExecutionRoutineParameterKind::rowset;
    case PolymorphicRoutineTypeKind::anytable:
      return parameter_kind == ExecutionRoutineParameterKind::table_value;
    case PolymorphicRoutineTypeKind::anycursor:
      return parameter_kind == ExecutionRoutineParameterKind::cursor;
  }
  return false;
}

inline bool ExecutionTypeDomainStackEquals(
    const ExecutionTypeDescriptor& left,
    const ExecutionTypeDescriptor& right) noexcept {
  if (left.domain_stack.size() != right.domain_stack.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.domain_stack.size(); ++index) {
    if (!ExecutionDataPacketUuidEquals(left.domain_stack[index],
                                       right.domain_stack[index])) {
      return false;
    }
  }
  return true;
}

struct PolymorphicRoutineBindingGroupState {
  Uuid binding_group_uuid{};
  ExecutionRoutineParameterKind parameter_kind =
      ExecutionRoutineParameterKind::scalar;
  PolymorphicRoutineBindingMatchPolicy match_policy =
      PolymorphicRoutineBindingMatchPolicy::same_type;
  ExecutionTypeDescriptor scalar_descriptor;
  ExecutionRelationDescriptor relation_descriptor;
};

inline PolymorphicRoutineBindingValidationResult
ValidatePolymorphicRoutineBinding(
    const ExecutionRoutineSignatureDescriptor& signature,
    const PolymorphicRoutineBindingDescriptor& binding) {
  if (ExecutionDataPacketUuidIsNil(binding.binding_uuid)) {
    return {PolymorphicRoutineBindingStatus::binding_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(binding.routine_signature_uuid)) {
    return {PolymorphicRoutineBindingStatus::routine_signature_uuid_required};
  }
  if (binding.binding_epoch == 0) {
    return {PolymorphicRoutineBindingStatus::binding_epoch_required};
  }
  if (binding.signature_epoch == 0) {
    return {PolymorphicRoutineBindingStatus::signature_epoch_required};
  }
  if (binding.stable_name.empty()) {
    return {PolymorphicRoutineBindingStatus::stable_name_required};
  }
  if (!binding.descriptor_authoritative) {
    return {PolymorphicRoutineBindingStatus::descriptor_not_authoritative};
  }
  if (!binding.parser_independent) {
    return {PolymorphicRoutineBindingStatus::descriptor_parser_dependent};
  }

  const auto signature_result =
      ValidateExecutionRoutineSignatureDescriptor(signature);
  if (!signature_result.ok()) {
    return {PolymorphicRoutineBindingStatus::signature_descriptor_invalid,
            signature_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(binding.routine_signature_uuid,
                                     signature.routine_signature_uuid)) {
    return {PolymorphicRoutineBindingStatus::signature_uuid_mismatch};
  }
  if (binding.signature_epoch != signature.signature_epoch) {
    return {PolymorphicRoutineBindingStatus::signature_epoch_mismatch};
  }
  if (binding.slots.size() != signature.parameter_descriptors.size()) {
    return {PolymorphicRoutineBindingStatus::slot_count_mismatch};
  }

  std::vector<PolymorphicRoutineBindingGroupState> groups;
  groups.reserve(binding.slots.size());
  for (std::size_t slot_index = 0; slot_index < binding.slots.size();
       ++slot_index) {
    const auto& slot = binding.slots[slot_index];
    const auto& parameter = signature.parameter_descriptors[slot_index];
    if (slot.ordinal != slot_index) {
      return {PolymorphicRoutineBindingStatus::slot_ordinal_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (slot.stable_name != parameter.stable_name) {
      return {PolymorphicRoutineBindingStatus::slot_name_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (slot.parameter_kind != parameter.parameter_kind) {
      return {PolymorphicRoutineBindingStatus::slot_parameter_kind_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (!PolymorphicRoutineTypeKindIsValid(slot.polymorphic_kind)) {
      return {PolymorphicRoutineBindingStatus::polymorphic_kind_invalid,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (!PolymorphicRoutineBindingMatchPolicyIsValid(slot.match_policy)) {
      return {PolymorphicRoutineBindingStatus::binding_policy_invalid,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (!PolymorphicRoutineTypeKindAcceptsParameter(slot.polymorphic_kind,
                                                   slot.parameter_kind)) {
      return {PolymorphicRoutineBindingStatus::polymorphic_kind_incompatible,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (slot.polymorphic_kind != PolymorphicRoutineTypeKind::concrete &&
        ExecutionDataPacketUuidIsNil(slot.binding_group_uuid)) {
      return {PolymorphicRoutineBindingStatus::binding_group_uuid_required,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }

    if (slot.parameter_kind == ExecutionRoutineParameterKind::scalar) {
      const auto scalar_result =
          ValidateExecutionDataPacketDescriptor(slot.scalar_descriptor,
                                                slot_index);
      if (!scalar_result.ok()) {
        return {PolymorphicRoutineBindingStatus::scalar_descriptor_invalid,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok, scalar_result.status,
                slot_index};
      }
      if (slot.polymorphic_kind == PolymorphicRoutineTypeKind::concrete &&
          !ExecutionTypeDescriptorIdentityEquals(parameter.scalar_descriptor,
                                                slot.scalar_descriptor)) {
        return {PolymorphicRoutineBindingStatus::
                    concrete_scalar_descriptor_mismatch,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, slot_index};
      }
    } else {
      const auto relation_result =
          ValidateExecutionRelationDescriptor(slot.relation_descriptor);
      if (!relation_result.ok()) {
        return {PolymorphicRoutineBindingStatus::relation_descriptor_invalid,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                relation_result.status, ExecutionDataPacketStatus::ok,
                slot_index};
      }
      if ((slot.parameter_kind == ExecutionRoutineParameterKind::cursor &&
           slot.relation_descriptor.relation_kind !=
               ExecutionRelationKind::cursor) ||
          (slot.parameter_kind == ExecutionRoutineParameterKind::rowset &&
           slot.relation_descriptor.relation_kind !=
               ExecutionRelationKind::rowset) ||
          (slot.parameter_kind == ExecutionRoutineParameterKind::table_value &&
           slot.relation_descriptor.relation_kind !=
               ExecutionRelationKind::table_value)) {
        return {PolymorphicRoutineBindingStatus::relation_kind_mismatch,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, slot_index};
      }
      if (slot.polymorphic_kind == PolymorphicRoutineTypeKind::concrete &&
          !ExecutionRelationDescriptorIdentityEquals(
              parameter.relation_descriptor, slot.relation_descriptor)) {
        return {PolymorphicRoutineBindingStatus::
                    concrete_relation_descriptor_mismatch,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, slot_index};
      }
    }

    if (slot.polymorphic_kind == PolymorphicRoutineTypeKind::concrete) {
      continue;
    }

    PolymorphicRoutineBindingGroupState* existing_group = nullptr;
    for (auto& group : groups) {
      if (ExecutionDataPacketUuidEquals(group.binding_group_uuid,
                                        slot.binding_group_uuid)) {
        existing_group = &group;
        break;
      }
    }
    if (existing_group == nullptr) {
      PolymorphicRoutineBindingGroupState group;
      group.binding_group_uuid = slot.binding_group_uuid;
      group.parameter_kind = slot.parameter_kind;
      group.match_policy = slot.match_policy;
      group.scalar_descriptor = slot.scalar_descriptor;
      group.relation_descriptor = slot.relation_descriptor;
      groups.push_back(group);
      continue;
    }
    if (existing_group->parameter_kind != slot.parameter_kind) {
      return {PolymorphicRoutineBindingStatus::binding_group_kind_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (existing_group->match_policy != slot.match_policy) {
      return {PolymorphicRoutineBindingStatus::binding_group_policy_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
    if (slot.parameter_kind == ExecutionRoutineParameterKind::scalar) {
      if (slot.match_policy == PolymorphicRoutineBindingMatchPolicy::same_type) {
        if (!ExecutionTypeDescriptorIdentityEquals(
                existing_group->scalar_descriptor, slot.scalar_descriptor)) {
          return {PolymorphicRoutineBindingStatus::
                      binding_group_descriptor_mismatch,
                  ExecutionRoutineSignatureDescriptorStatus::ok,
                  ExecutionRelationDescriptorStatus::ok,
                  ExecutionDataPacketStatus::ok, slot_index};
        }
      } else if (!ExecutionTypeDomainStackEquals(
                     existing_group->scalar_descriptor,
                     slot.scalar_descriptor)) {
        return {PolymorphicRoutineBindingStatus::
                    binding_group_domain_stack_mismatch,
                ExecutionRoutineSignatureDescriptorStatus::ok,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionDataPacketStatus::ok, slot_index};
      }
    } else if (!ExecutionRelationDescriptorIdentityEquals(
                   existing_group->relation_descriptor,
                   slot.relation_descriptor)) {
      return {PolymorphicRoutineBindingStatus::
                  binding_group_descriptor_mismatch,
              ExecutionRoutineSignatureDescriptorStatus::ok,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionDataPacketStatus::ok, slot_index};
    }
  }
  return {};
}

// SEARCH_KEY: comparison key
// SEARCH_KEY: null ordering
// SEARCH_KEY: collation key
enum class ExecutionComparisonKeyKind : std::uint8_t {
  scalar = 0,
  numeric = 1,
  temporal = 2,
  text_collation = 3,
  domain = 4,
  reference_compatible = 5
};

constexpr bool ExecutionComparisonKeyKindIsValid(
    ExecutionComparisonKeyKind kind) noexcept {
  switch (kind) {
    case ExecutionComparisonKeyKind::scalar:
    case ExecutionComparisonKeyKind::numeric:
    case ExecutionComparisonKeyKind::temporal:
    case ExecutionComparisonKeyKind::text_collation:
    case ExecutionComparisonKeyKind::domain:
    case ExecutionComparisonKeyKind::reference_compatible:
      return true;
  }
  return false;
}

enum class ExecutionComparisonSortDirection : std::uint8_t {
  ascending = 0,
  descending = 1
};

constexpr bool ExecutionComparisonSortDirectionIsValid(
    ExecutionComparisonSortDirection direction) noexcept {
  switch (direction) {
    case ExecutionComparisonSortDirection::ascending:
    case ExecutionComparisonSortDirection::descending:
      return true;
  }
  return false;
}

enum class ExecutionComparisonNullOrdering : std::uint8_t {
  nulls_first = 0,
  nulls_last = 1
};

constexpr bool ExecutionComparisonNullOrderingIsValid(
    ExecutionComparisonNullOrdering ordering) noexcept {
  switch (ordering) {
    case ExecutionComparisonNullOrdering::nulls_first:
    case ExecutionComparisonNullOrdering::nulls_last:
      return true;
  }
  return false;
}

struct ExecutionComparisonKeyDescriptor {
  Uuid comparison_key_uuid{};
  Uuid type_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  ExecutionComparisonKeyKind key_kind = ExecutionComparisonKeyKind::scalar;
  ExecutionComparisonSortDirection sort_direction =
      ExecutionComparisonSortDirection::ascending;
  ExecutionComparisonNullOrdering null_ordering =
      ExecutionComparisonNullOrdering::nulls_last;
  ExecutionValueState value_state = ExecutionValueState::value;
  ExecutionTypeDescriptor type_descriptor;
  Uuid collation_uuid{};
  Uuid timezone_uuid{};
  Uuid domain_uuid{};
  std::string reference_profile_name;
  std::vector<std::uint8_t> canonical_payload;
  bool lossy = false;
  bool requires_recheck = false;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionComparisonKeyStatus : std::uint8_t {
  ok = 0,
  comparison_key_uuid_required = 1,
  type_descriptor_uuid_required = 2,
  descriptor_epoch_required = 3,
  stable_name_required = 4,
  descriptor_not_authoritative = 5,
  descriptor_parser_dependent = 6,
  key_kind_invalid = 7,
  sort_direction_invalid = 8,
  null_ordering_invalid = 9,
  type_descriptor_invalid = 10,
  type_descriptor_uuid_mismatch = 11,
  type_descriptor_epoch_mismatch = 12,
  value_state_invalid = 13,
  null_payload_not_allowed = 14,
  payload_required = 15,
  numeric_family_required = 16,
  temporal_family_required = 17,
  text_family_required = 18,
  collation_uuid_required = 19,
  collation_uuid_mismatch = 20,
  timezone_uuid_required = 21,
  timezone_uuid_mismatch = 22,
  domain_flag_required = 23,
  domain_uuid_required = 24,
  domain_uuid_mismatch = 25,
  reference_profile_required = 26,
  reference_recheck_required = 27
};

constexpr std::string_view ExecutionComparisonKeyStatusName(
    ExecutionComparisonKeyStatus status) noexcept {
  switch (status) {
    case ExecutionComparisonKeyStatus::ok:
      return "ok";
    case ExecutionComparisonKeyStatus::comparison_key_uuid_required:
      return "comparison_key_uuid_required";
    case ExecutionComparisonKeyStatus::type_descriptor_uuid_required:
      return "type_descriptor_uuid_required";
    case ExecutionComparisonKeyStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionComparisonKeyStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionComparisonKeyStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionComparisonKeyStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionComparisonKeyStatus::key_kind_invalid:
      return "key_kind_invalid";
    case ExecutionComparisonKeyStatus::sort_direction_invalid:
      return "sort_direction_invalid";
    case ExecutionComparisonKeyStatus::null_ordering_invalid:
      return "null_ordering_invalid";
    case ExecutionComparisonKeyStatus::type_descriptor_invalid:
      return "type_descriptor_invalid";
    case ExecutionComparisonKeyStatus::type_descriptor_uuid_mismatch:
      return "type_descriptor_uuid_mismatch";
    case ExecutionComparisonKeyStatus::type_descriptor_epoch_mismatch:
      return "type_descriptor_epoch_mismatch";
    case ExecutionComparisonKeyStatus::value_state_invalid:
      return "value_state_invalid";
    case ExecutionComparisonKeyStatus::null_payload_not_allowed:
      return "null_payload_not_allowed";
    case ExecutionComparisonKeyStatus::payload_required:
      return "payload_required";
    case ExecutionComparisonKeyStatus::numeric_family_required:
      return "numeric_family_required";
    case ExecutionComparisonKeyStatus::temporal_family_required:
      return "temporal_family_required";
    case ExecutionComparisonKeyStatus::text_family_required:
      return "text_family_required";
    case ExecutionComparisonKeyStatus::collation_uuid_required:
      return "collation_uuid_required";
    case ExecutionComparisonKeyStatus::collation_uuid_mismatch:
      return "collation_uuid_mismatch";
    case ExecutionComparisonKeyStatus::timezone_uuid_required:
      return "timezone_uuid_required";
    case ExecutionComparisonKeyStatus::timezone_uuid_mismatch:
      return "timezone_uuid_mismatch";
    case ExecutionComparisonKeyStatus::domain_flag_required:
      return "domain_flag_required";
    case ExecutionComparisonKeyStatus::domain_uuid_required:
      return "domain_uuid_required";
    case ExecutionComparisonKeyStatus::domain_uuid_mismatch:
      return "domain_uuid_mismatch";
    case ExecutionComparisonKeyStatus::reference_profile_required:
      return "reference_profile_required";
    case ExecutionComparisonKeyStatus::reference_recheck_required:
      return "reference_recheck_required";
  }
  return "unknown_status";
}

struct ExecutionComparisonKeyValidationResult {
  ExecutionComparisonKeyStatus status = ExecutionComparisonKeyStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionComparisonKeyStatus::ok;
  }
};

constexpr bool ExecutionComparisonKeyNumericFamily(
    ExecutionTypeFamily family) noexcept {
  switch (family) {
    case ExecutionTypeFamily::signed_integer:
    case ExecutionTypeFamily::unsigned_integer:
    case ExecutionTypeFamily::real:
    case ExecutionTypeFamily::decimal:
      return true;
    default:
      return false;
  }
}

constexpr bool ExecutionComparisonKeyTextFamily(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::character;
}

inline bool ExecutionComparisonKeyHasModifierFlag(
    const ExecutionTypeDescriptor& descriptor,
    ExecutionTypeModifierFlag flag) noexcept {
  return (descriptor.modifier_flags & ExecutionTypeModifierFlagBit(flag)) != 0;
}

inline ExecutionComparisonKeyValidationResult
ValidateExecutionComparisonKeyDescriptor(
    const ExecutionComparisonKeyDescriptor& key) {
  if (ExecutionDataPacketUuidIsNil(key.comparison_key_uuid)) {
    return {ExecutionComparisonKeyStatus::comparison_key_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(key.type_descriptor_uuid)) {
    return {ExecutionComparisonKeyStatus::type_descriptor_uuid_required};
  }
  if (key.descriptor_epoch == 0) {
    return {ExecutionComparisonKeyStatus::descriptor_epoch_required};
  }
  if (key.stable_name.empty()) {
    return {ExecutionComparisonKeyStatus::stable_name_required};
  }
  if (!key.descriptor_authoritative) {
    return {ExecutionComparisonKeyStatus::descriptor_not_authoritative};
  }
  if (!key.parser_independent) {
    return {ExecutionComparisonKeyStatus::descriptor_parser_dependent};
  }
  if (!ExecutionComparisonKeyKindIsValid(key.key_kind)) {
    return {ExecutionComparisonKeyStatus::key_kind_invalid};
  }
  if (!ExecutionComparisonSortDirectionIsValid(key.sort_direction)) {
    return {ExecutionComparisonKeyStatus::sort_direction_invalid};
  }
  if (!ExecutionComparisonNullOrderingIsValid(key.null_ordering)) {
    return {ExecutionComparisonKeyStatus::null_ordering_invalid};
  }

  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(key.type_descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionComparisonKeyStatus::type_descriptor_invalid,
            descriptor_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(key.type_descriptor.descriptor_uuid,
                                     key.type_descriptor_uuid)) {
    return {ExecutionComparisonKeyStatus::type_descriptor_uuid_mismatch};
  }
  if (key.type_descriptor.descriptor_epoch != key.descriptor_epoch) {
    return {ExecutionComparisonKeyStatus::type_descriptor_epoch_mismatch};
  }

  switch (key.value_state) {
    case ExecutionValueState::value:
    case ExecutionValueState::lob_handle:
    case ExecutionValueState::protected_value:
      if (key.canonical_payload.empty()) {
        return {ExecutionComparisonKeyStatus::payload_required};
      }
      break;
    case ExecutionValueState::sql_null:
      if (!key.canonical_payload.empty()) {
        return {ExecutionComparisonKeyStatus::null_payload_not_allowed};
      }
      return {};
    case ExecutionValueState::missing:
    case ExecutionValueState::default_requested:
    case ExecutionValueState::unknown:
    case ExecutionValueState::error:
      return {ExecutionComparisonKeyStatus::value_state_invalid};
  }

  if (key.key_kind == ExecutionComparisonKeyKind::numeric &&
      !ExecutionComparisonKeyNumericFamily(key.type_descriptor.family)) {
    return {ExecutionComparisonKeyStatus::numeric_family_required};
  }
  if (key.key_kind == ExecutionComparisonKeyKind::temporal) {
    if (key.type_descriptor.family != ExecutionTypeFamily::temporal) {
      return {ExecutionComparisonKeyStatus::temporal_family_required};
    }
    if (ExecutionComparisonKeyHasModifierFlag(
            key.type_descriptor, ExecutionTypeModifierFlag::timezone_uuid)) {
      if (ExecutionDataPacketUuidIsNil(key.timezone_uuid)) {
        return {ExecutionComparisonKeyStatus::timezone_uuid_required};
      }
      if (!ExecutionDataPacketUuidEquals(key.type_descriptor.timezone_uuid,
                                         key.timezone_uuid)) {
        return {ExecutionComparisonKeyStatus::timezone_uuid_mismatch};
      }
    }
  }
  if (key.key_kind == ExecutionComparisonKeyKind::text_collation) {
    if (key.type_descriptor.family != ExecutionTypeFamily::character) {
      return {ExecutionComparisonKeyStatus::text_family_required};
    }
    if (ExecutionDataPacketUuidIsNil(key.collation_uuid)) {
      return {ExecutionComparisonKeyStatus::collation_uuid_required};
    }
    if (ExecutionComparisonKeyHasModifierFlag(
            key.type_descriptor, ExecutionTypeModifierFlag::collation_uuid) &&
        !ExecutionDataPacketUuidEquals(key.type_descriptor.collation_uuid,
                                       key.collation_uuid)) {
      return {ExecutionComparisonKeyStatus::collation_uuid_mismatch};
    }
  }
  if (key.key_kind == ExecutionComparisonKeyKind::domain) {
    if (!ExecutionComparisonKeyHasModifierFlag(
            key.type_descriptor, ExecutionTypeModifierFlag::domain_uuid)) {
      return {ExecutionComparisonKeyStatus::domain_flag_required};
    }
    if (ExecutionDataPacketUuidIsNil(key.domain_uuid)) {
      return {ExecutionComparisonKeyStatus::domain_uuid_required};
    }
    if (!ExecutionDataPacketUuidEquals(key.type_descriptor.domain_uuid,
                                       key.domain_uuid)) {
      return {ExecutionComparisonKeyStatus::domain_uuid_mismatch};
    }
  }
  if (key.key_kind == ExecutionComparisonKeyKind::reference_compatible) {
    if (key.reference_profile_name.empty()) {
      return {ExecutionComparisonKeyStatus::reference_profile_required};
    }
    if (key.lossy && !key.requires_recheck) {
      return {ExecutionComparisonKeyStatus::reference_recheck_required};
    }
  }
  return {};
}

// SEARCH_KEY: rounding mode
// SEARCH_KEY: signed zero
// SEARCH_KEY: NaN
// SEARCH_KEY: decimal overflow
enum class ExecutionNumericRoundingMode : std::uint8_t {
  half_even = 0,
  half_up = 1,
  toward_zero = 2,
  floor = 3,
  ceiling = 4
};

constexpr bool ExecutionNumericRoundingModeIsValid(
    ExecutionNumericRoundingMode mode) noexcept {
  switch (mode) {
    case ExecutionNumericRoundingMode::half_even:
    case ExecutionNumericRoundingMode::half_up:
    case ExecutionNumericRoundingMode::toward_zero:
    case ExecutionNumericRoundingMode::floor:
    case ExecutionNumericRoundingMode::ceiling:
      return true;
  }
  return false;
}

enum class ExecutionNumericOverflowPolicy : std::uint8_t {
  error = 0,
  saturate = 1,
  reference_compatible = 2
};

constexpr bool ExecutionNumericOverflowPolicyIsValid(
    ExecutionNumericOverflowPolicy policy) noexcept {
  switch (policy) {
    case ExecutionNumericOverflowPolicy::error:
    case ExecutionNumericOverflowPolicy::saturate:
    case ExecutionNumericOverflowPolicy::reference_compatible:
      return true;
  }
  return false;
}

enum class ExecutionNumericSpecialValuePolicy : std::uint8_t {
  reject = 0,
  allow_with_descriptor = 1,
  reference_compatible = 2
};

constexpr bool ExecutionNumericSpecialValuePolicyIsValid(
    ExecutionNumericSpecialValuePolicy policy) noexcept {
  switch (policy) {
    case ExecutionNumericSpecialValuePolicy::reject:
    case ExecutionNumericSpecialValuePolicy::allow_with_descriptor:
    case ExecutionNumericSpecialValuePolicy::reference_compatible:
      return true;
  }
  return false;
}

enum class ExecutionNumericSignedZeroPolicy : std::uint8_t {
  canonicalize_positive = 0,
  preserve = 1,
  reject = 2
};

constexpr bool ExecutionNumericSignedZeroPolicyIsValid(
    ExecutionNumericSignedZeroPolicy policy) noexcept {
  switch (policy) {
    case ExecutionNumericSignedZeroPolicy::canonicalize_positive:
    case ExecutionNumericSignedZeroPolicy::preserve:
    case ExecutionNumericSignedZeroPolicy::reject:
      return true;
  }
  return false;
}

struct ExecutionNumericEdgePolicyDescriptor {
  Uuid policy_uuid{};
  Uuid type_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  ExecutionTypeDescriptor type_descriptor;
  ExecutionNumericRoundingMode rounding_mode =
      ExecutionNumericRoundingMode::half_even;
  ExecutionNumericOverflowPolicy overflow_policy =
      ExecutionNumericOverflowPolicy::error;
  ExecutionNumericSpecialValuePolicy nan_policy =
      ExecutionNumericSpecialValuePolicy::reject;
  ExecutionNumericSpecialValuePolicy infinity_policy =
      ExecutionNumericSpecialValuePolicy::reject;
  ExecutionNumericSignedZeroPolicy signed_zero_policy =
      ExecutionNumericSignedZeroPolicy::canonicalize_positive;
  std::uint32_t precision = 0;
  std::uint32_t scale = 0;
  bool decimal_floating_context = false;
  std::uint32_t decimal_context_precision = 0;
  std::string reference_profile_name;
  bool reference_difference_documented = false;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionNumericEdgePolicyStatus : std::uint8_t {
  ok = 0,
  policy_uuid_required = 1,
  type_descriptor_uuid_required = 2,
  descriptor_epoch_required = 3,
  stable_name_required = 4,
  descriptor_not_authoritative = 5,
  descriptor_parser_dependent = 6,
  rounding_mode_invalid = 7,
  overflow_policy_invalid = 8,
  nan_policy_invalid = 9,
  infinity_policy_invalid = 10,
  signed_zero_policy_invalid = 11,
  type_descriptor_invalid = 12,
  type_descriptor_uuid_mismatch = 13,
  type_descriptor_epoch_mismatch = 14,
  numeric_family_required = 15,
  precision_mismatch = 16,
  scale_mismatch = 17,
  scale_exceeds_precision = 18,
  decimal_context_required = 19,
  signed_zero_not_supported = 20,
  reference_profile_required = 21,
  reference_difference_required = 22
};

constexpr std::string_view ExecutionNumericEdgePolicyStatusName(
    ExecutionNumericEdgePolicyStatus status) noexcept {
  switch (status) {
    case ExecutionNumericEdgePolicyStatus::ok:
      return "ok";
    case ExecutionNumericEdgePolicyStatus::policy_uuid_required:
      return "policy_uuid_required";
    case ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_required:
      return "type_descriptor_uuid_required";
    case ExecutionNumericEdgePolicyStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionNumericEdgePolicyStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionNumericEdgePolicyStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionNumericEdgePolicyStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionNumericEdgePolicyStatus::rounding_mode_invalid:
      return "rounding_mode_invalid";
    case ExecutionNumericEdgePolicyStatus::overflow_policy_invalid:
      return "overflow_policy_invalid";
    case ExecutionNumericEdgePolicyStatus::nan_policy_invalid:
      return "nan_policy_invalid";
    case ExecutionNumericEdgePolicyStatus::infinity_policy_invalid:
      return "infinity_policy_invalid";
    case ExecutionNumericEdgePolicyStatus::signed_zero_policy_invalid:
      return "signed_zero_policy_invalid";
    case ExecutionNumericEdgePolicyStatus::type_descriptor_invalid:
      return "type_descriptor_invalid";
    case ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_mismatch:
      return "type_descriptor_uuid_mismatch";
    case ExecutionNumericEdgePolicyStatus::type_descriptor_epoch_mismatch:
      return "type_descriptor_epoch_mismatch";
    case ExecutionNumericEdgePolicyStatus::numeric_family_required:
      return "numeric_family_required";
    case ExecutionNumericEdgePolicyStatus::precision_mismatch:
      return "precision_mismatch";
    case ExecutionNumericEdgePolicyStatus::scale_mismatch:
      return "scale_mismatch";
    case ExecutionNumericEdgePolicyStatus::scale_exceeds_precision:
      return "scale_exceeds_precision";
    case ExecutionNumericEdgePolicyStatus::decimal_context_required:
      return "decimal_context_required";
    case ExecutionNumericEdgePolicyStatus::signed_zero_not_supported:
      return "signed_zero_not_supported";
    case ExecutionNumericEdgePolicyStatus::reference_profile_required:
      return "reference_profile_required";
    case ExecutionNumericEdgePolicyStatus::reference_difference_required:
      return "reference_difference_required";
  }
  return "unknown_status";
}

struct ExecutionNumericEdgePolicyValidationResult {
  ExecutionNumericEdgePolicyStatus status =
      ExecutionNumericEdgePolicyStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionNumericEdgePolicyStatus::ok;
  }
};

constexpr bool ExecutionNumericEdgePolicyUsesReference(
    const ExecutionNumericEdgePolicyDescriptor& policy) noexcept {
  return policy.overflow_policy ==
             ExecutionNumericOverflowPolicy::reference_compatible ||
         policy.nan_policy ==
             ExecutionNumericSpecialValuePolicy::reference_compatible ||
         policy.infinity_policy ==
             ExecutionNumericSpecialValuePolicy::reference_compatible;
}

inline ExecutionNumericEdgePolicyValidationResult
ValidateExecutionNumericEdgePolicyDescriptor(
    const ExecutionNumericEdgePolicyDescriptor& policy) {
  if (ExecutionDataPacketUuidIsNil(policy.policy_uuid)) {
    return {ExecutionNumericEdgePolicyStatus::policy_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(policy.type_descriptor_uuid)) {
    return {ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_required};
  }
  if (policy.descriptor_epoch == 0) {
    return {ExecutionNumericEdgePolicyStatus::descriptor_epoch_required};
  }
  if (policy.stable_name.empty()) {
    return {ExecutionNumericEdgePolicyStatus::stable_name_required};
  }
  if (!policy.descriptor_authoritative) {
    return {ExecutionNumericEdgePolicyStatus::descriptor_not_authoritative};
  }
  if (!policy.parser_independent) {
    return {ExecutionNumericEdgePolicyStatus::descriptor_parser_dependent};
  }
  if (!ExecutionNumericRoundingModeIsValid(policy.rounding_mode)) {
    return {ExecutionNumericEdgePolicyStatus::rounding_mode_invalid};
  }
  if (!ExecutionNumericOverflowPolicyIsValid(policy.overflow_policy)) {
    return {ExecutionNumericEdgePolicyStatus::overflow_policy_invalid};
  }
  if (!ExecutionNumericSpecialValuePolicyIsValid(policy.nan_policy)) {
    return {ExecutionNumericEdgePolicyStatus::nan_policy_invalid};
  }
  if (!ExecutionNumericSpecialValuePolicyIsValid(policy.infinity_policy)) {
    return {ExecutionNumericEdgePolicyStatus::infinity_policy_invalid};
  }
  if (!ExecutionNumericSignedZeroPolicyIsValid(policy.signed_zero_policy)) {
    return {ExecutionNumericEdgePolicyStatus::signed_zero_policy_invalid};
  }

  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(policy.type_descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionNumericEdgePolicyStatus::type_descriptor_invalid,
            descriptor_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(policy.type_descriptor.descriptor_uuid,
                                     policy.type_descriptor_uuid)) {
    return {ExecutionNumericEdgePolicyStatus::type_descriptor_uuid_mismatch};
  }
  if (policy.type_descriptor.descriptor_epoch != policy.descriptor_epoch) {
    return {ExecutionNumericEdgePolicyStatus::type_descriptor_epoch_mismatch};
  }
  if (!ExecutionComparisonKeyNumericFamily(policy.type_descriptor.family)) {
    return {ExecutionNumericEdgePolicyStatus::numeric_family_required};
  }
  if (ExecutionComparisonKeyHasModifierFlag(
          policy.type_descriptor, ExecutionTypeModifierFlag::precision) &&
      policy.type_descriptor.precision != policy.precision) {
    return {ExecutionNumericEdgePolicyStatus::precision_mismatch};
  }
  if (ExecutionComparisonKeyHasModifierFlag(
          policy.type_descriptor, ExecutionTypeModifierFlag::scale) &&
      policy.type_descriptor.scale != policy.scale) {
    return {ExecutionNumericEdgePolicyStatus::scale_mismatch};
  }
  if (policy.precision != 0 && policy.scale > policy.precision) {
    return {ExecutionNumericEdgePolicyStatus::scale_exceeds_precision};
  }
  const bool special_value_allowed =
      policy.nan_policy ==
          ExecutionNumericSpecialValuePolicy::allow_with_descriptor ||
      policy.infinity_policy ==
          ExecutionNumericSpecialValuePolicy::allow_with_descriptor ||
      policy.nan_policy ==
          ExecutionNumericSpecialValuePolicy::reference_compatible ||
      policy.infinity_policy ==
          ExecutionNumericSpecialValuePolicy::reference_compatible;
  if ((policy.decimal_floating_context || special_value_allowed) &&
      policy.decimal_context_precision == 0) {
    return {ExecutionNumericEdgePolicyStatus::decimal_context_required};
  }
  if (policy.type_descriptor.family == ExecutionTypeFamily::unsigned_integer &&
      policy.signed_zero_policy == ExecutionNumericSignedZeroPolicy::preserve) {
    return {ExecutionNumericEdgePolicyStatus::signed_zero_not_supported};
  }
  if (ExecutionNumericEdgePolicyUsesReference(policy)) {
    if (policy.reference_profile_name.empty()) {
      return {ExecutionNumericEdgePolicyStatus::reference_profile_required};
    }
    if (!policy.reference_difference_documented) {
      return {ExecutionNumericEdgePolicyStatus::reference_difference_required};
    }
  }
  return {};
}

// SEARCH_KEY: descriptor version
// SEARCH_KEY: descriptor migration
// SEARCH_KEY: descriptor snapshot
inline constexpr std::size_t kDescriptorMigrationMaxArtifacts = 4096;

enum class DescriptorMigrationArtifactKind : std::uint8_t {
  cache_entry = 0,
  cursor_handle = 1,
  rowset_value = 2,
  data_packet = 3,
  index_metadata = 4
};

constexpr bool DescriptorMigrationArtifactKindIsValid(
    DescriptorMigrationArtifactKind kind) noexcept {
  switch (kind) {
    case DescriptorMigrationArtifactKind::cache_entry:
    case DescriptorMigrationArtifactKind::cursor_handle:
    case DescriptorMigrationArtifactKind::rowset_value:
    case DescriptorMigrationArtifactKind::data_packet:
    case DescriptorMigrationArtifactKind::index_metadata:
      return true;
  }
  return false;
}

enum class DescriptorMigrationAction : std::uint8_t {
  invalidate = 0,
  migrate = 1,
  preserve = 2
};

constexpr bool DescriptorMigrationActionIsValid(
    DescriptorMigrationAction action) noexcept {
  switch (action) {
    case DescriptorMigrationAction::invalidate:
    case DescriptorMigrationAction::migrate:
    case DescriptorMigrationAction::preserve:
      return true;
  }
  return false;
}

struct DescriptorMigrationArtifactDescriptor {
  Uuid artifact_uuid{};
  DescriptorMigrationArtifactKind artifact_kind =
      DescriptorMigrationArtifactKind::cache_entry;
  DescriptorMigrationAction action = DescriptorMigrationAction::invalidate;
  Uuid source_descriptor_uuid{};
  std::uint64_t source_descriptor_epoch = 0;
  Uuid target_descriptor_uuid{};
  std::uint64_t target_descriptor_epoch = 0;
  Uuid artifact_snapshot_uuid{};
  Uuid migration_proof_uuid{};
  bool invalidation_required = false;
  bool migration_supported = false;
  bool epoch_independent = false;
};

struct DescriptorMigrationPlanDescriptor {
  Uuid migration_uuid{};
  Uuid descriptor_uuid{};
  std::uint64_t source_descriptor_epoch = 0;
  std::uint64_t target_descriptor_epoch = 0;
  Uuid descriptor_snapshot_uuid{};
  std::string stable_name;
  std::vector<DescriptorMigrationArtifactDescriptor> artifacts;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class DescriptorMigrationStatus : std::uint8_t {
  ok = 0,
  migration_uuid_required = 1,
  descriptor_uuid_required = 2,
  source_epoch_required = 3,
  target_epoch_required = 4,
  target_epoch_not_newer = 5,
  descriptor_snapshot_uuid_required = 6,
  stable_name_required = 7,
  descriptor_not_authoritative = 8,
  descriptor_parser_dependent = 9,
  artifacts_required = 10,
  artifact_count_exceeds_limit = 11,
  artifact_uuid_required = 12,
  artifact_kind_invalid = 13,
  action_invalid = 14,
  artifact_source_uuid_mismatch = 15,
  artifact_source_epoch_mismatch = 16,
  artifact_target_uuid_mismatch = 17,
  artifact_target_epoch_mismatch = 18,
  invalidate_flag_required = 19,
  migration_not_supported = 20,
  migration_proof_required = 21,
  artifact_snapshot_required = 22,
  preserve_without_epoch_independent_proof = 23
};

constexpr std::string_view DescriptorMigrationStatusName(
    DescriptorMigrationStatus status) noexcept {
  switch (status) {
    case DescriptorMigrationStatus::ok:
      return "ok";
    case DescriptorMigrationStatus::migration_uuid_required:
      return "migration_uuid_required";
    case DescriptorMigrationStatus::descriptor_uuid_required:
      return "descriptor_uuid_required";
    case DescriptorMigrationStatus::source_epoch_required:
      return "source_epoch_required";
    case DescriptorMigrationStatus::target_epoch_required:
      return "target_epoch_required";
    case DescriptorMigrationStatus::target_epoch_not_newer:
      return "target_epoch_not_newer";
    case DescriptorMigrationStatus::descriptor_snapshot_uuid_required:
      return "descriptor_snapshot_uuid_required";
    case DescriptorMigrationStatus::stable_name_required:
      return "stable_name_required";
    case DescriptorMigrationStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case DescriptorMigrationStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case DescriptorMigrationStatus::artifacts_required:
      return "artifacts_required";
    case DescriptorMigrationStatus::artifact_count_exceeds_limit:
      return "artifact_count_exceeds_limit";
    case DescriptorMigrationStatus::artifact_uuid_required:
      return "artifact_uuid_required";
    case DescriptorMigrationStatus::artifact_kind_invalid:
      return "artifact_kind_invalid";
    case DescriptorMigrationStatus::action_invalid:
      return "action_invalid";
    case DescriptorMigrationStatus::artifact_source_uuid_mismatch:
      return "artifact_source_uuid_mismatch";
    case DescriptorMigrationStatus::artifact_source_epoch_mismatch:
      return "artifact_source_epoch_mismatch";
    case DescriptorMigrationStatus::artifact_target_uuid_mismatch:
      return "artifact_target_uuid_mismatch";
    case DescriptorMigrationStatus::artifact_target_epoch_mismatch:
      return "artifact_target_epoch_mismatch";
    case DescriptorMigrationStatus::invalidate_flag_required:
      return "invalidate_flag_required";
    case DescriptorMigrationStatus::migration_not_supported:
      return "migration_not_supported";
    case DescriptorMigrationStatus::migration_proof_required:
      return "migration_proof_required";
    case DescriptorMigrationStatus::artifact_snapshot_required:
      return "artifact_snapshot_required";
    case DescriptorMigrationStatus::preserve_without_epoch_independent_proof:
      return "preserve_without_epoch_independent_proof";
  }
  return "unknown_status";
}

struct DescriptorMigrationValidationResult {
  DescriptorMigrationStatus status = DescriptorMigrationStatus::ok;
  std::size_t artifact_index = 0;

  bool ok() const noexcept {
    return status == DescriptorMigrationStatus::ok;
  }
};

inline DescriptorMigrationValidationResult
ValidateDescriptorMigrationPlan(
    const DescriptorMigrationPlanDescriptor& plan) {
  if (ExecutionDataPacketUuidIsNil(plan.migration_uuid)) {
    return {DescriptorMigrationStatus::migration_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(plan.descriptor_uuid)) {
    return {DescriptorMigrationStatus::descriptor_uuid_required};
  }
  if (plan.source_descriptor_epoch == 0) {
    return {DescriptorMigrationStatus::source_epoch_required};
  }
  if (plan.target_descriptor_epoch == 0) {
    return {DescriptorMigrationStatus::target_epoch_required};
  }
  if (plan.target_descriptor_epoch <= plan.source_descriptor_epoch) {
    return {DescriptorMigrationStatus::target_epoch_not_newer};
  }
  if (ExecutionDataPacketUuidIsNil(plan.descriptor_snapshot_uuid)) {
    return {DescriptorMigrationStatus::descriptor_snapshot_uuid_required};
  }
  if (plan.stable_name.empty()) {
    return {DescriptorMigrationStatus::stable_name_required};
  }
  if (!plan.descriptor_authoritative) {
    return {DescriptorMigrationStatus::descriptor_not_authoritative};
  }
  if (!plan.parser_independent) {
    return {DescriptorMigrationStatus::descriptor_parser_dependent};
  }
  if (plan.artifacts.empty()) {
    return {DescriptorMigrationStatus::artifacts_required};
  }
  if (plan.artifacts.size() > kDescriptorMigrationMaxArtifacts) {
    return {DescriptorMigrationStatus::artifact_count_exceeds_limit};
  }

  for (std::size_t index = 0; index < plan.artifacts.size(); ++index) {
    const auto& artifact = plan.artifacts[index];
    if (ExecutionDataPacketUuidIsNil(artifact.artifact_uuid)) {
      return {DescriptorMigrationStatus::artifact_uuid_required, index};
    }
    if (!DescriptorMigrationArtifactKindIsValid(artifact.artifact_kind)) {
      return {DescriptorMigrationStatus::artifact_kind_invalid, index};
    }
    if (!DescriptorMigrationActionIsValid(artifact.action)) {
      return {DescriptorMigrationStatus::action_invalid, index};
    }
    if (!ExecutionDataPacketUuidEquals(artifact.source_descriptor_uuid,
                                       plan.descriptor_uuid)) {
      return {DescriptorMigrationStatus::artifact_source_uuid_mismatch, index};
    }
    if (artifact.source_descriptor_epoch != plan.source_descriptor_epoch) {
      return {DescriptorMigrationStatus::artifact_source_epoch_mismatch, index};
    }
    if (!ExecutionDataPacketUuidEquals(artifact.target_descriptor_uuid,
                                       plan.descriptor_uuid)) {
      return {DescriptorMigrationStatus::artifact_target_uuid_mismatch, index};
    }
    if (artifact.target_descriptor_epoch != plan.target_descriptor_epoch) {
      return {DescriptorMigrationStatus::artifact_target_epoch_mismatch, index};
    }

    switch (artifact.action) {
      case DescriptorMigrationAction::invalidate:
        if (!artifact.invalidation_required) {
          return {DescriptorMigrationStatus::invalidate_flag_required, index};
        }
        break;
      case DescriptorMigrationAction::migrate:
        if (!artifact.migration_supported) {
          return {DescriptorMigrationStatus::migration_not_supported, index};
        }
        if (ExecutionDataPacketUuidIsNil(artifact.migration_proof_uuid)) {
          return {DescriptorMigrationStatus::migration_proof_required, index};
        }
        if (ExecutionDataPacketUuidIsNil(artifact.artifact_snapshot_uuid)) {
          return {DescriptorMigrationStatus::artifact_snapshot_required, index};
        }
        break;
      case DescriptorMigrationAction::preserve:
        if (!artifact.epoch_independent) {
          return {DescriptorMigrationStatus::
                      preserve_without_epoch_independent_proof,
                  index};
        }
        if (ExecutionDataPacketUuidIsNil(artifact.artifact_snapshot_uuid)) {
          return {DescriptorMigrationStatus::artifact_snapshot_required, index};
        }
        break;
    }
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_OWNERSHIP_TRANSFER
enum class ExecutionOwnershipContextKind : std::uint8_t {
  client = 0,
  server = 1,
  routine = 2,
  trigger = 3,
  worker = 4
};

constexpr bool ExecutionOwnershipContextKindIsValid(
    ExecutionOwnershipContextKind context_kind) noexcept {
  switch (context_kind) {
    case ExecutionOwnershipContextKind::client:
    case ExecutionOwnershipContextKind::server:
    case ExecutionOwnershipContextKind::routine:
    case ExecutionOwnershipContextKind::trigger:
    case ExecutionOwnershipContextKind::worker:
      return true;
  }
  return false;
}

enum class ExecutionOwnershipTransferMode : std::uint8_t {
  pass_by_value = 0,
  pass_by_reference = 1,
  detach = 2,
  cleanup = 3,
  close = 4
};

constexpr bool ExecutionOwnershipTransferModeIsValid(
    ExecutionOwnershipTransferMode transfer_mode) noexcept {
  switch (transfer_mode) {
    case ExecutionOwnershipTransferMode::pass_by_value:
    case ExecutionOwnershipTransferMode::pass_by_reference:
    case ExecutionOwnershipTransferMode::detach:
    case ExecutionOwnershipTransferMode::cleanup:
    case ExecutionOwnershipTransferMode::close:
      return true;
  }
  return false;
}

struct ExecutionOwnershipTransferRecord {
  Uuid transfer_uuid{};
  Uuid handle_uuid{};
  ExecutionRelationDescriptor relation_descriptor;
  ExecutionOwnershipContextKind source_context =
      ExecutionOwnershipContextKind::server;
  ExecutionOwnershipContextKind target_context =
      ExecutionOwnershipContextKind::routine;
  Uuid source_owner_uuid{};
  Uuid target_owner_uuid{};
  Uuid source_transaction_uuid{};
  Uuid target_transaction_uuid{};
  PortalState before_state = PortalState::open;
  PortalState after_state = PortalState::open;
  ExecutionOwnershipTransferMode transfer_mode =
      ExecutionOwnershipTransferMode::pass_by_reference;
  std::uint64_t transfer_epoch = 0;
  bool close_owner = false;
  bool cleanup_required = false;
  bool audit_required = true;
  Uuid audit_event_uuid{};
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionOwnershipTransferStatus : std::uint8_t {
  ok = 0,
  transfer_uuid_required = 1,
  handle_uuid_required = 2,
  relation_descriptor_invalid = 3,
  relation_kind_invalid = 4,
  transfer_epoch_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  source_context_invalid = 8,
  target_context_invalid = 9,
  source_owner_uuid_required = 10,
  target_owner_uuid_required = 11,
  source_transaction_uuid_required = 12,
  target_transaction_uuid_required = 13,
  before_state_invalid = 14,
  after_state_invalid = 15,
  transfer_mode_invalid = 16,
  closed_source_not_transferable = 17,
  pass_by_reference_close_owner_forbidden = 18,
  pass_by_value_close_owner_required = 19,
  detach_state_required = 20,
  cleanup_owner_required = 21,
  close_state_required = 22,
  audit_event_uuid_required = 23
};

constexpr std::string_view ExecutionOwnershipTransferStatusName(
    ExecutionOwnershipTransferStatus status) noexcept {
  switch (status) {
    case ExecutionOwnershipTransferStatus::ok:
      return "ok";
    case ExecutionOwnershipTransferStatus::transfer_uuid_required:
      return "transfer_uuid_required";
    case ExecutionOwnershipTransferStatus::handle_uuid_required:
      return "handle_uuid_required";
    case ExecutionOwnershipTransferStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionOwnershipTransferStatus::relation_kind_invalid:
      return "relation_kind_invalid";
    case ExecutionOwnershipTransferStatus::transfer_epoch_required:
      return "transfer_epoch_required";
    case ExecutionOwnershipTransferStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionOwnershipTransferStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionOwnershipTransferStatus::source_context_invalid:
      return "source_context_invalid";
    case ExecutionOwnershipTransferStatus::target_context_invalid:
      return "target_context_invalid";
    case ExecutionOwnershipTransferStatus::source_owner_uuid_required:
      return "source_owner_uuid_required";
    case ExecutionOwnershipTransferStatus::target_owner_uuid_required:
      return "target_owner_uuid_required";
    case ExecutionOwnershipTransferStatus::source_transaction_uuid_required:
      return "source_transaction_uuid_required";
    case ExecutionOwnershipTransferStatus::target_transaction_uuid_required:
      return "target_transaction_uuid_required";
    case ExecutionOwnershipTransferStatus::before_state_invalid:
      return "before_state_invalid";
    case ExecutionOwnershipTransferStatus::after_state_invalid:
      return "after_state_invalid";
    case ExecutionOwnershipTransferStatus::transfer_mode_invalid:
      return "transfer_mode_invalid";
    case ExecutionOwnershipTransferStatus::closed_source_not_transferable:
      return "closed_source_not_transferable";
    case ExecutionOwnershipTransferStatus::
        pass_by_reference_close_owner_forbidden:
      return "pass_by_reference_close_owner_forbidden";
    case ExecutionOwnershipTransferStatus::pass_by_value_close_owner_required:
      return "pass_by_value_close_owner_required";
    case ExecutionOwnershipTransferStatus::detach_state_required:
      return "detach_state_required";
    case ExecutionOwnershipTransferStatus::cleanup_owner_required:
      return "cleanup_owner_required";
    case ExecutionOwnershipTransferStatus::close_state_required:
      return "close_state_required";
    case ExecutionOwnershipTransferStatus::audit_event_uuid_required:
      return "audit_event_uuid_required";
  }
  return "unknown_status";
}

struct ExecutionOwnershipTransferValidationResult {
  ExecutionOwnershipTransferStatus status =
      ExecutionOwnershipTransferStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionOwnershipTransferStatus::ok;
  }
};

inline bool ExecutionOwnershipTransferRelationKindIsAllowed(
    ExecutionRelationKind relation_kind) noexcept {
  switch (relation_kind) {
    case ExecutionRelationKind::cursor:
    case ExecutionRelationKind::rowset:
    case ExecutionRelationKind::table_value:
    case ExecutionRelationKind::result_channel:
      return true;
    case ExecutionRelationKind::remote_fragment:
      return false;
  }
  return false;
}

inline ExecutionOwnershipTransferValidationResult
ValidateExecutionOwnershipTransferRecord(
    const ExecutionOwnershipTransferRecord& record) {
  if (ExecutionDataPacketUuidIsNil(record.transfer_uuid)) {
    return {ExecutionOwnershipTransferStatus::transfer_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(record.handle_uuid)) {
    return {ExecutionOwnershipTransferStatus::handle_uuid_required};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(record.relation_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionOwnershipTransferStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (!ExecutionOwnershipTransferRelationKindIsAllowed(
          record.relation_descriptor.relation_kind)) {
    return {ExecutionOwnershipTransferStatus::relation_kind_invalid};
  }
  if (record.transfer_epoch == 0) {
    return {ExecutionOwnershipTransferStatus::transfer_epoch_required};
  }
  if (!record.descriptor_authoritative) {
    return {ExecutionOwnershipTransferStatus::descriptor_not_authoritative};
  }
  if (!record.parser_independent) {
    return {ExecutionOwnershipTransferStatus::descriptor_parser_dependent};
  }
  if (!ExecutionOwnershipContextKindIsValid(record.source_context)) {
    return {ExecutionOwnershipTransferStatus::source_context_invalid};
  }
  if (!ExecutionOwnershipContextKindIsValid(record.target_context)) {
    return {ExecutionOwnershipTransferStatus::target_context_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(record.source_owner_uuid)) {
    return {ExecutionOwnershipTransferStatus::source_owner_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(record.target_owner_uuid)) {
    return {ExecutionOwnershipTransferStatus::target_owner_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(record.source_transaction_uuid)) {
    return {ExecutionOwnershipTransferStatus::source_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(record.target_transaction_uuid)) {
    return {ExecutionOwnershipTransferStatus::target_transaction_uuid_required};
  }
  if (!PortalStateIsValid(record.before_state)) {
    return {ExecutionOwnershipTransferStatus::before_state_invalid};
  }
  if (!PortalStateIsValid(record.after_state)) {
    return {ExecutionOwnershipTransferStatus::after_state_invalid};
  }
  if (!ExecutionOwnershipTransferModeIsValid(record.transfer_mode)) {
    return {ExecutionOwnershipTransferStatus::transfer_mode_invalid};
  }
  if (record.before_state == PortalState::closed) {
    return {ExecutionOwnershipTransferStatus::closed_source_not_transferable};
  }

  switch (record.transfer_mode) {
    case ExecutionOwnershipTransferMode::pass_by_reference:
      if (record.close_owner || record.cleanup_required) {
        return {
            ExecutionOwnershipTransferStatus::
                pass_by_reference_close_owner_forbidden};
      }
      break;
    case ExecutionOwnershipTransferMode::pass_by_value:
      if (!record.close_owner) {
        return {
            ExecutionOwnershipTransferStatus::pass_by_value_close_owner_required};
      }
      break;
    case ExecutionOwnershipTransferMode::detach:
      if (record.after_state != PortalState::detached || !record.close_owner) {
        return {ExecutionOwnershipTransferStatus::detach_state_required};
      }
      break;
    case ExecutionOwnershipTransferMode::cleanup:
      if (record.after_state != PortalState::cleanup_pending ||
          !record.close_owner || !record.cleanup_required) {
        return {ExecutionOwnershipTransferStatus::cleanup_owner_required};
      }
      break;
    case ExecutionOwnershipTransferMode::close:
      if (record.after_state != PortalState::closed || !record.close_owner) {
        return {ExecutionOwnershipTransferStatus::close_state_required};
      }
      break;
  }

  if (record.audit_required &&
      ExecutionDataPacketUuidIsNil(record.audit_event_uuid)) {
    return {ExecutionOwnershipTransferStatus::audit_event_uuid_required};
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_RESULT_CHANNEL_DESCRIPTOR
enum class ExecutionResultChannelRenderingPolicy : std::uint8_t {
  ordered_stream = 0,
  nested_envelope = 1,
  diagnostic_side_channel = 2,
  hidden_internal = 3
};

constexpr bool ExecutionResultChannelRenderingPolicyIsValid(
    ExecutionResultChannelRenderingPolicy policy) noexcept {
  switch (policy) {
    case ExecutionResultChannelRenderingPolicy::ordered_stream:
    case ExecutionResultChannelRenderingPolicy::nested_envelope:
    case ExecutionResultChannelRenderingPolicy::diagnostic_side_channel:
    case ExecutionResultChannelRenderingPolicy::hidden_internal:
      return true;
  }
  return false;
}

struct ExecutionResultChannelDescriptor {
  Uuid result_channel_uuid{};
  std::uint32_t ordinal = 0;
  std::string stable_name;
  ExecutionRelationDescriptor relation_descriptor;
  Uuid parent_result_channel_uuid{};
  std::uint32_t nesting_depth = 0;
  ExecutionResultChannelRenderingPolicy rendering_policy =
      ExecutionResultChannelRenderingPolicy::ordered_stream;
  bool default_channel = false;
  bool public_rendering = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct ExecutionResultChannelSetDescriptor {
  Uuid result_channel_set_uuid{};
  std::uint64_t descriptor_epoch = 0;
  Uuid routine_uuid{};
  std::string stable_name;
  std::vector<ExecutionResultChannelDescriptor> channels;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool allow_multiple_channels = true;
  bool allow_nested_channels = true;
};

enum class ExecutionResultChannelSetStatus : std::uint8_t {
  ok = 0,
  channel_set_uuid_required = 1,
  descriptor_epoch_required = 2,
  routine_uuid_required = 3,
  stable_name_required = 4,
  descriptor_not_authoritative = 5,
  descriptor_parser_dependent = 6,
  channels_required = 7,
  channel_uuid_required = 8,
  channel_ordinal_mismatch = 9,
  channel_name_required = 10,
  channel_descriptor_not_authoritative = 11,
  channel_descriptor_parser_dependent = 12,
  relation_descriptor_invalid = 13,
  relation_kind_invalid = 14,
  rendering_policy_invalid = 15,
  hidden_public_channel = 16,
  multiple_channels_not_allowed = 17,
  nested_channels_not_allowed = 18,
  parent_channel_not_found = 19,
  parent_channel_order_invalid = 20,
  nesting_depth_mismatch = 21,
  nested_rendering_policy_required = 22,
  duplicate_default_channel = 23,
  routine_signature_invalid = 24,
  routine_uuid_mismatch = 25,
  routine_result_count_mismatch = 26,
  routine_result_name_mismatch = 27,
  routine_result_descriptor_mismatch = 28
};

constexpr std::string_view ExecutionResultChannelSetStatusName(
    ExecutionResultChannelSetStatus status) noexcept {
  switch (status) {
    case ExecutionResultChannelSetStatus::ok:
      return "ok";
    case ExecutionResultChannelSetStatus::channel_set_uuid_required:
      return "channel_set_uuid_required";
    case ExecutionResultChannelSetStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionResultChannelSetStatus::routine_uuid_required:
      return "routine_uuid_required";
    case ExecutionResultChannelSetStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionResultChannelSetStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionResultChannelSetStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionResultChannelSetStatus::channels_required:
      return "channels_required";
    case ExecutionResultChannelSetStatus::channel_uuid_required:
      return "channel_uuid_required";
    case ExecutionResultChannelSetStatus::channel_ordinal_mismatch:
      return "channel_ordinal_mismatch";
    case ExecutionResultChannelSetStatus::channel_name_required:
      return "channel_name_required";
    case ExecutionResultChannelSetStatus::channel_descriptor_not_authoritative:
      return "channel_descriptor_not_authoritative";
    case ExecutionResultChannelSetStatus::channel_descriptor_parser_dependent:
      return "channel_descriptor_parser_dependent";
    case ExecutionResultChannelSetStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionResultChannelSetStatus::relation_kind_invalid:
      return "relation_kind_invalid";
    case ExecutionResultChannelSetStatus::rendering_policy_invalid:
      return "rendering_policy_invalid";
    case ExecutionResultChannelSetStatus::hidden_public_channel:
      return "hidden_public_channel";
    case ExecutionResultChannelSetStatus::multiple_channels_not_allowed:
      return "multiple_channels_not_allowed";
    case ExecutionResultChannelSetStatus::nested_channels_not_allowed:
      return "nested_channels_not_allowed";
    case ExecutionResultChannelSetStatus::parent_channel_not_found:
      return "parent_channel_not_found";
    case ExecutionResultChannelSetStatus::parent_channel_order_invalid:
      return "parent_channel_order_invalid";
    case ExecutionResultChannelSetStatus::nesting_depth_mismatch:
      return "nesting_depth_mismatch";
    case ExecutionResultChannelSetStatus::nested_rendering_policy_required:
      return "nested_rendering_policy_required";
    case ExecutionResultChannelSetStatus::duplicate_default_channel:
      return "duplicate_default_channel";
    case ExecutionResultChannelSetStatus::routine_signature_invalid:
      return "routine_signature_invalid";
    case ExecutionResultChannelSetStatus::routine_uuid_mismatch:
      return "routine_uuid_mismatch";
    case ExecutionResultChannelSetStatus::routine_result_count_mismatch:
      return "routine_result_count_mismatch";
    case ExecutionResultChannelSetStatus::routine_result_name_mismatch:
      return "routine_result_name_mismatch";
    case ExecutionResultChannelSetStatus::routine_result_descriptor_mismatch:
      return "routine_result_descriptor_mismatch";
  }
  return "unknown_status";
}

struct ExecutionResultChannelSetValidationResult {
  ExecutionResultChannelSetStatus status = ExecutionResultChannelSetStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;
  ExecutionRoutineSignatureDescriptorStatus routine_signature_status =
      ExecutionRoutineSignatureDescriptorStatus::ok;
  std::size_t channel_index = 0;
  std::size_t routine_result_index = 0;

  bool ok() const noexcept {
    return status == ExecutionResultChannelSetStatus::ok;
  }
};

inline std::size_t ExecutionResultChannelTopLevelCount(
    const ExecutionResultChannelSetDescriptor& descriptor) noexcept {
  std::size_t count = 0;
  for (const auto& channel : descriptor.channels) {
    if (ExecutionDataPacketUuidIsNil(channel.parent_result_channel_uuid)) {
      ++count;
    }
  }
  return count;
}

inline ExecutionResultChannelSetValidationResult
ValidateExecutionResultChannelSetDescriptor(
    const ExecutionResultChannelSetDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.result_channel_set_uuid)) {
    return {ExecutionResultChannelSetStatus::channel_set_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {ExecutionResultChannelSetStatus::descriptor_epoch_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.routine_uuid)) {
    return {ExecutionResultChannelSetStatus::routine_uuid_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ExecutionResultChannelSetStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionResultChannelSetStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionResultChannelSetStatus::descriptor_parser_dependent};
  }
  if (descriptor.channels.empty()) {
    return {ExecutionResultChannelSetStatus::channels_required};
  }
  if (!descriptor.allow_multiple_channels && descriptor.channels.size() > 1) {
    return {ExecutionResultChannelSetStatus::multiple_channels_not_allowed};
  }

  bool default_channel_seen = false;
  for (std::size_t channel_index = 0;
       channel_index < descriptor.channels.size(); ++channel_index) {
    const auto& channel = descriptor.channels[channel_index];
    if (ExecutionDataPacketUuidIsNil(channel.result_channel_uuid)) {
      return {ExecutionResultChannelSetStatus::channel_uuid_required,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.ordinal != channel_index) {
      return {ExecutionResultChannelSetStatus::channel_ordinal_mismatch,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.stable_name.empty()) {
      return {ExecutionResultChannelSetStatus::channel_name_required,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (!channel.descriptor_authoritative) {
      return {
          ExecutionResultChannelSetStatus::channel_descriptor_not_authoritative,
          ExecutionRelationDescriptorStatus::ok,
          ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (!channel.parser_independent) {
      return {
          ExecutionResultChannelSetStatus::channel_descriptor_parser_dependent,
          ExecutionRelationDescriptorStatus::ok,
          ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    const auto relation_result =
        ValidateExecutionRelationDescriptor(channel.relation_descriptor);
    if (!relation_result.ok()) {
      return {ExecutionResultChannelSetStatus::relation_descriptor_invalid,
              relation_result.status,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.relation_descriptor.relation_kind !=
        ExecutionRelationKind::result_channel) {
      return {ExecutionResultChannelSetStatus::relation_kind_invalid,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (!ExecutionResultChannelRenderingPolicyIsValid(
            channel.rendering_policy)) {
      return {ExecutionResultChannelSetStatus::rendering_policy_invalid,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.public_rendering &&
        channel.rendering_policy ==
            ExecutionResultChannelRenderingPolicy::hidden_internal) {
      return {ExecutionResultChannelSetStatus::hidden_public_channel,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.default_channel) {
      if (default_channel_seen) {
        return {ExecutionResultChannelSetStatus::duplicate_default_channel,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionRoutineSignatureDescriptorStatus::ok, channel_index,
                0};
      }
      default_channel_seen = true;
    }

    if (ExecutionDataPacketUuidIsNil(channel.parent_result_channel_uuid)) {
      if (channel.nesting_depth != 0) {
        return {ExecutionResultChannelSetStatus::nesting_depth_mismatch,
                ExecutionRelationDescriptorStatus::ok,
                ExecutionRoutineSignatureDescriptorStatus::ok, channel_index,
                0};
      }
      continue;
    }

    if (!descriptor.allow_nested_channels) {
      return {ExecutionResultChannelSetStatus::nested_channels_not_allowed,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.rendering_policy !=
        ExecutionResultChannelRenderingPolicy::nested_envelope) {
      return {ExecutionResultChannelSetStatus::nested_rendering_policy_required,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }

    bool parent_found = false;
    std::size_t parent_index = 0;
    for (std::size_t candidate_index = 0;
         candidate_index < descriptor.channels.size(); ++candidate_index) {
      if (ExecutionDataPacketUuidEquals(
              descriptor.channels[candidate_index].result_channel_uuid,
              channel.parent_result_channel_uuid)) {
        parent_found = true;
        parent_index = candidate_index;
        break;
      }
    }
    if (!parent_found) {
      return {ExecutionResultChannelSetStatus::parent_channel_not_found,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (parent_index >= channel_index) {
      return {ExecutionResultChannelSetStatus::parent_channel_order_invalid,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
    if (channel.nesting_depth !=
        descriptor.channels[parent_index].nesting_depth + 1) {
      return {ExecutionResultChannelSetStatus::nesting_depth_mismatch,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index, 0};
    }
  }

  return {};
}

inline ExecutionResultChannelSetValidationResult
ValidateExecutionResultChannelSetForRoutineSignature(
    const ExecutionRoutineSignatureDescriptor& signature,
    const ExecutionResultChannelSetDescriptor& descriptor) {
  const auto signature_result =
      ValidateExecutionRoutineSignatureDescriptor(signature);
  if (!signature_result.ok()) {
    return {ExecutionResultChannelSetStatus::routine_signature_invalid,
            ExecutionRelationDescriptorStatus::ok, signature_result.status};
  }

  const auto descriptor_result =
      ValidateExecutionResultChannelSetDescriptor(descriptor);
  if (!descriptor_result.ok()) {
    return descriptor_result;
  }
  if (!ExecutionDataPacketUuidEquals(signature.routine_uuid,
                                     descriptor.routine_uuid)) {
    return {ExecutionResultChannelSetStatus::routine_uuid_mismatch};
  }

  const std::size_t top_level_count =
      ExecutionResultChannelTopLevelCount(descriptor);
  if (top_level_count != signature.result_descriptors.size()) {
    return {ExecutionResultChannelSetStatus::routine_result_count_mismatch};
  }

  std::size_t routine_result_index = 0;
  for (std::size_t channel_index = 0;
       channel_index < descriptor.channels.size(); ++channel_index) {
    const auto& channel = descriptor.channels[channel_index];
    if (!ExecutionDataPacketUuidIsNil(channel.parent_result_channel_uuid)) {
      continue;
    }
    const auto& result = signature.result_descriptors[routine_result_index];
    if (channel.stable_name != result.stable_name) {
      return {ExecutionResultChannelSetStatus::routine_result_name_mismatch,
              ExecutionRelationDescriptorStatus::ok,
              ExecutionRoutineSignatureDescriptorStatus::ok, channel_index,
              routine_result_index};
    }
    if (!ExecutionRelationDescriptorIdentityEquals(channel.relation_descriptor,
                                                  result.relation_descriptor)) {
      return {
          ExecutionResultChannelSetStatus::routine_result_descriptor_mismatch,
          ExecutionRelationDescriptorStatus::ok,
          ExecutionRoutineSignatureDescriptorStatus::ok, channel_index,
          routine_result_index};
    }
    ++routine_result_index;
  }

  return {};
}

// SEARCH_KEY: EDR_EXECUTION_OPTIMIZER_SAFETY_DESCRIPTOR
enum class ExecutionOptimizerProducerKind : std::uint8_t {
  cursor = 0,
  rowset = 1,
  table_value = 2,
  result_channel = 3,
  remote_fragment = 4,
  sblr_operator = 5,
  routine = 6,
  udr = 7
};

constexpr bool ExecutionOptimizerProducerKindIsValid(
    ExecutionOptimizerProducerKind producer_kind) noexcept {
  switch (producer_kind) {
    case ExecutionOptimizerProducerKind::cursor:
    case ExecutionOptimizerProducerKind::rowset:
    case ExecutionOptimizerProducerKind::table_value:
    case ExecutionOptimizerProducerKind::result_channel:
    case ExecutionOptimizerProducerKind::remote_fragment:
    case ExecutionOptimizerProducerKind::sblr_operator:
    case ExecutionOptimizerProducerKind::routine:
    case ExecutionOptimizerProducerKind::udr:
      return true;
  }
  return false;
}

enum class ExecutionProducerRewindability : std::uint8_t {
  rewindable = 0,
  forward_only = 1,
  materialized_snapshot = 2
};

constexpr bool ExecutionProducerRewindabilityIsValid(
    ExecutionProducerRewindability rewindability) noexcept {
  switch (rewindability) {
    case ExecutionProducerRewindability::rewindable:
    case ExecutionProducerRewindability::forward_only:
    case ExecutionProducerRewindability::materialized_snapshot:
      return true;
  }
  return false;
}

enum class ExecutionProducerDeterminism : std::uint8_t {
  snapshot_deterministic = 0,
  statement_deterministic = 1,
  transaction_volatile = 2,
  nondeterministic = 3
};

constexpr bool ExecutionProducerDeterminismIsValid(
    ExecutionProducerDeterminism determinism) noexcept {
  switch (determinism) {
    case ExecutionProducerDeterminism::snapshot_deterministic:
    case ExecutionProducerDeterminism::statement_deterministic:
    case ExecutionProducerDeterminism::transaction_volatile:
    case ExecutionProducerDeterminism::nondeterministic:
      return true;
  }
  return false;
}

enum class ExecutionProducerSideEffectProfile : std::uint8_t {
  side_effect_free = 0,
  side_effecting = 1,
  external_effect = 2
};

constexpr bool ExecutionProducerSideEffectProfileIsValid(
    ExecutionProducerSideEffectProfile side_effect_profile) noexcept {
  switch (side_effect_profile) {
    case ExecutionProducerSideEffectProfile::side_effect_free:
    case ExecutionProducerSideEffectProfile::side_effecting:
    case ExecutionProducerSideEffectProfile::external_effect:
      return true;
  }
  return false;
}

enum class ExecutionOptimizerConsumerMode : std::uint8_t {
  single_pass = 0,
  rescan = 1,
  duplicate = 2,
  cache = 3,
  parallel_fanout = 4
};

constexpr bool ExecutionOptimizerConsumerModeIsValid(
    ExecutionOptimizerConsumerMode consumer_mode) noexcept {
  switch (consumer_mode) {
    case ExecutionOptimizerConsumerMode::single_pass:
    case ExecutionOptimizerConsumerMode::rescan:
    case ExecutionOptimizerConsumerMode::duplicate:
    case ExecutionOptimizerConsumerMode::cache:
    case ExecutionOptimizerConsumerMode::parallel_fanout:
      return true;
  }
  return false;
}

struct ExecutionOptimizerProducerDescriptor {
  Uuid optimizer_producer_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  ExecutionOptimizerProducerKind producer_kind =
      ExecutionOptimizerProducerKind::sblr_operator;
  ExecutionRelationDescriptor relation_descriptor;
  Uuid owner_transaction_uuid{};
  Uuid snapshot_uuid{};
  Uuid security_policy_uuid{};
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  ExecutionProducerRewindability rewindability =
      ExecutionProducerRewindability::rewindable;
  ExecutionProducerDeterminism determinism =
      ExecutionProducerDeterminism::snapshot_deterministic;
  ExecutionProducerSideEffectProfile side_effect_profile =
      ExecutionProducerSideEffectProfile::side_effect_free;
  bool materialization_allowed = true;
  bool cache_result_allowed = true;
  bool parallel_fanout_allowed = true;
};

struct ExecutionOptimizerAccessPlan {
  Uuid plan_uuid{};
  std::uint64_t optimizer_epoch = 0;
  ExecutionOptimizerConsumerMode consumer_mode =
      ExecutionOptimizerConsumerMode::single_pass;
  std::uint32_t rescan_count = 0;
  std::uint32_t duplicate_count = 1;
  bool uses_materialized_spool = false;
  bool uses_descriptor_bound_cache_key = false;
  bool duplicates_producer_execution = false;
  bool parallel_fanout = false;
  bool executor_contract_acknowledged = true;
};

enum class ExecutionOptimizerPlanAdmissionStatus : std::uint8_t {
  ok = 0,
  producer_uuid_required = 1,
  descriptor_epoch_required = 2,
  stable_name_required = 3,
  descriptor_not_authoritative = 4,
  descriptor_parser_dependent = 5,
  producer_kind_invalid = 6,
  relation_descriptor_invalid = 7,
  relation_kind_invalid = 8,
  owner_transaction_uuid_required = 9,
  snapshot_uuid_required = 10,
  security_policy_uuid_required = 11,
  rewindability_invalid = 12,
  determinism_invalid = 13,
  side_effect_profile_invalid = 14,
  plan_uuid_required = 15,
  optimizer_epoch_required = 16,
  consumer_mode_invalid = 17,
  executor_contract_required = 18,
  materialization_not_allowed = 19,
  non_rewindable_rescan_refused = 20,
  nondeterministic_rescan_refused = 21,
  non_rewindable_duplicate_refused = 22,
  nondeterministic_duplicate_refused = 23,
  side_effecting_duplicate_refused = 24,
  cache_not_allowed = 25,
  cache_requires_descriptor_bound_key = 26,
  cache_requires_snapshot_determinism = 27,
  side_effecting_cache_refused = 28,
  parallel_fanout_not_allowed = 29,
  parallel_requires_materialized_or_rewindable = 30,
  parallel_requires_snapshot_determinism = 31,
  side_effecting_parallel_refused = 32
};

constexpr std::string_view ExecutionOptimizerPlanAdmissionStatusName(
    ExecutionOptimizerPlanAdmissionStatus status) noexcept {
  switch (status) {
    case ExecutionOptimizerPlanAdmissionStatus::ok:
      return "ok";
    case ExecutionOptimizerPlanAdmissionStatus::producer_uuid_required:
      return "producer_uuid_required";
    case ExecutionOptimizerPlanAdmissionStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionOptimizerPlanAdmissionStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionOptimizerPlanAdmissionStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionOptimizerPlanAdmissionStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionOptimizerPlanAdmissionStatus::producer_kind_invalid:
      return "producer_kind_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::relation_kind_invalid:
      return "relation_kind_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionOptimizerPlanAdmissionStatus::snapshot_uuid_required:
      return "snapshot_uuid_required";
    case ExecutionOptimizerPlanAdmissionStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionOptimizerPlanAdmissionStatus::rewindability_invalid:
      return "rewindability_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::determinism_invalid:
      return "determinism_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::side_effect_profile_invalid:
      return "side_effect_profile_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::plan_uuid_required:
      return "plan_uuid_required";
    case ExecutionOptimizerPlanAdmissionStatus::optimizer_epoch_required:
      return "optimizer_epoch_required";
    case ExecutionOptimizerPlanAdmissionStatus::consumer_mode_invalid:
      return "consumer_mode_invalid";
    case ExecutionOptimizerPlanAdmissionStatus::executor_contract_required:
      return "executor_contract_required";
    case ExecutionOptimizerPlanAdmissionStatus::materialization_not_allowed:
      return "materialization_not_allowed";
    case ExecutionOptimizerPlanAdmissionStatus::non_rewindable_rescan_refused:
      return "non_rewindable_rescan_refused";
    case ExecutionOptimizerPlanAdmissionStatus::nondeterministic_rescan_refused:
      return "nondeterministic_rescan_refused";
    case ExecutionOptimizerPlanAdmissionStatus::non_rewindable_duplicate_refused:
      return "non_rewindable_duplicate_refused";
    case ExecutionOptimizerPlanAdmissionStatus::nondeterministic_duplicate_refused:
      return "nondeterministic_duplicate_refused";
    case ExecutionOptimizerPlanAdmissionStatus::side_effecting_duplicate_refused:
      return "side_effecting_duplicate_refused";
    case ExecutionOptimizerPlanAdmissionStatus::cache_not_allowed:
      return "cache_not_allowed";
    case ExecutionOptimizerPlanAdmissionStatus::
        cache_requires_descriptor_bound_key:
      return "cache_requires_descriptor_bound_key";
    case ExecutionOptimizerPlanAdmissionStatus::
        cache_requires_snapshot_determinism:
      return "cache_requires_snapshot_determinism";
    case ExecutionOptimizerPlanAdmissionStatus::side_effecting_cache_refused:
      return "side_effecting_cache_refused";
    case ExecutionOptimizerPlanAdmissionStatus::parallel_fanout_not_allowed:
      return "parallel_fanout_not_allowed";
    case ExecutionOptimizerPlanAdmissionStatus::
        parallel_requires_materialized_or_rewindable:
      return "parallel_requires_materialized_or_rewindable";
    case ExecutionOptimizerPlanAdmissionStatus::
        parallel_requires_snapshot_determinism:
      return "parallel_requires_snapshot_determinism";
    case ExecutionOptimizerPlanAdmissionStatus::side_effecting_parallel_refused:
      return "side_effecting_parallel_refused";
  }
  return "unknown_status";
}

struct ExecutionOptimizerPlanAdmissionResult {
  ExecutionOptimizerPlanAdmissionStatus status =
      ExecutionOptimizerPlanAdmissionStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionOptimizerPlanAdmissionStatus::ok;
  }
};

constexpr bool ExecutionOptimizerProducerKindAllowsRelationKind(
    ExecutionOptimizerProducerKind producer_kind,
    ExecutionRelationKind relation_kind) noexcept {
  switch (producer_kind) {
    case ExecutionOptimizerProducerKind::cursor:
      return relation_kind == ExecutionRelationKind::cursor;
    case ExecutionOptimizerProducerKind::rowset:
      return relation_kind == ExecutionRelationKind::rowset;
    case ExecutionOptimizerProducerKind::table_value:
      return relation_kind == ExecutionRelationKind::table_value;
    case ExecutionOptimizerProducerKind::result_channel:
      return relation_kind == ExecutionRelationKind::result_channel;
    case ExecutionOptimizerProducerKind::remote_fragment:
      return relation_kind == ExecutionRelationKind::remote_fragment;
    case ExecutionOptimizerProducerKind::sblr_operator:
    case ExecutionOptimizerProducerKind::routine:
    case ExecutionOptimizerProducerKind::udr:
      return ExecutionRelationKindIsValid(relation_kind);
  }
  return false;
}

constexpr bool ExecutionProducerIsSnapshotDeterministic(
    const ExecutionOptimizerProducerDescriptor& producer) noexcept {
  return producer.determinism ==
         ExecutionProducerDeterminism::snapshot_deterministic;
}

constexpr bool ExecutionProducerIsRewindable(
    const ExecutionOptimizerProducerDescriptor& producer) noexcept {
  return producer.rewindability == ExecutionProducerRewindability::rewindable ||
         producer.rewindability ==
             ExecutionProducerRewindability::materialized_snapshot;
}

constexpr bool ExecutionProducerHasSideEffects(
    const ExecutionOptimizerProducerDescriptor& producer) noexcept {
  return producer.side_effect_profile !=
         ExecutionProducerSideEffectProfile::side_effect_free;
}

inline ExecutionOptimizerPlanAdmissionResult
ValidateExecutionOptimizerProducerDescriptor(
    const ExecutionOptimizerProducerDescriptor& producer) {
  if (ExecutionDataPacketUuidIsNil(producer.optimizer_producer_uuid)) {
    return {ExecutionOptimizerPlanAdmissionStatus::producer_uuid_required};
  }
  if (producer.descriptor_epoch == 0) {
    return {ExecutionOptimizerPlanAdmissionStatus::descriptor_epoch_required};
  }
  if (producer.stable_name.empty()) {
    return {ExecutionOptimizerPlanAdmissionStatus::stable_name_required};
  }
  if (!producer.descriptor_authoritative) {
    return {ExecutionOptimizerPlanAdmissionStatus::descriptor_not_authoritative};
  }
  if (!producer.parser_independent) {
    return {ExecutionOptimizerPlanAdmissionStatus::descriptor_parser_dependent};
  }
  if (!ExecutionOptimizerProducerKindIsValid(producer.producer_kind)) {
    return {ExecutionOptimizerPlanAdmissionStatus::producer_kind_invalid};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(producer.relation_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionOptimizerPlanAdmissionStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (!ExecutionOptimizerProducerKindAllowsRelationKind(
          producer.producer_kind, producer.relation_descriptor.relation_kind)) {
    return {ExecutionOptimizerPlanAdmissionStatus::relation_kind_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(producer.owner_transaction_uuid)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::owner_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(producer.snapshot_uuid)) {
    return {ExecutionOptimizerPlanAdmissionStatus::snapshot_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(producer.security_policy_uuid)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::security_policy_uuid_required};
  }
  if (!ExecutionProducerRewindabilityIsValid(producer.rewindability)) {
    return {ExecutionOptimizerPlanAdmissionStatus::rewindability_invalid};
  }
  if (!ExecutionProducerDeterminismIsValid(producer.determinism)) {
    return {ExecutionOptimizerPlanAdmissionStatus::determinism_invalid};
  }
  if (!ExecutionProducerSideEffectProfileIsValid(
          producer.side_effect_profile)) {
    return {ExecutionOptimizerPlanAdmissionStatus::side_effect_profile_invalid};
  }
  return {};
}

inline ExecutionOptimizerPlanAdmissionResult
ValidateExecutionOptimizerPlanAdmission(
    const ExecutionOptimizerProducerDescriptor& producer,
    const ExecutionOptimizerAccessPlan& plan) {
  const auto producer_result =
      ValidateExecutionOptimizerProducerDescriptor(producer);
  if (!producer_result.ok()) {
    return producer_result;
  }

  if (ExecutionDataPacketUuidIsNil(plan.plan_uuid)) {
    return {ExecutionOptimizerPlanAdmissionStatus::plan_uuid_required};
  }
  if (plan.optimizer_epoch == 0) {
    return {ExecutionOptimizerPlanAdmissionStatus::optimizer_epoch_required};
  }
  if (!ExecutionOptimizerConsumerModeIsValid(plan.consumer_mode)) {
    return {ExecutionOptimizerPlanAdmissionStatus::consumer_mode_invalid};
  }
  if (!plan.executor_contract_acknowledged) {
    return {ExecutionOptimizerPlanAdmissionStatus::executor_contract_required};
  }
  if (plan.uses_materialized_spool && !producer.materialization_allowed) {
    return {ExecutionOptimizerPlanAdmissionStatus::materialization_not_allowed};
  }

  const bool uses_stable_replay =
      plan.uses_materialized_spool || plan.uses_descriptor_bound_cache_key;
  const bool requires_rescan =
      plan.consumer_mode == ExecutionOptimizerConsumerMode::rescan ||
      plan.rescan_count > 0;
  const bool duplicates_output =
      plan.consumer_mode == ExecutionOptimizerConsumerMode::duplicate ||
      plan.duplicate_count > 1;
  const bool uses_cache =
      plan.consumer_mode == ExecutionOptimizerConsumerMode::cache ||
      plan.uses_descriptor_bound_cache_key;
  const bool uses_parallel =
      plan.consumer_mode == ExecutionOptimizerConsumerMode::parallel_fanout ||
      plan.parallel_fanout;

  if (requires_rescan && !uses_stable_replay &&
      !ExecutionProducerIsRewindable(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::non_rewindable_rescan_refused};
  }
  if (requires_rescan && !uses_stable_replay &&
      !ExecutionProducerIsSnapshotDeterministic(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::nondeterministic_rescan_refused};
  }

  if (plan.duplicates_producer_execution &&
      ExecutionProducerHasSideEffects(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::side_effecting_duplicate_refused};
  }
  if (plan.duplicates_producer_execution &&
      !ExecutionProducerIsSnapshotDeterministic(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::nondeterministic_duplicate_refused};
  }
  if (duplicates_output && !uses_stable_replay &&
      !ExecutionProducerIsRewindable(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::non_rewindable_duplicate_refused};
  }
  if (duplicates_output && !uses_stable_replay &&
      !ExecutionProducerIsSnapshotDeterministic(producer)) {
    return {
        ExecutionOptimizerPlanAdmissionStatus::nondeterministic_duplicate_refused};
  }

  if (uses_cache) {
    if (!producer.cache_result_allowed) {
      return {ExecutionOptimizerPlanAdmissionStatus::cache_not_allowed};
    }
    if (!plan.uses_descriptor_bound_cache_key) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::
              cache_requires_descriptor_bound_key};
    }
    if (!ExecutionProducerIsSnapshotDeterministic(producer)) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::
              cache_requires_snapshot_determinism};
    }
    if (ExecutionProducerHasSideEffects(producer)) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::side_effecting_cache_refused};
    }
  }

  if (uses_parallel) {
    if (!producer.parallel_fanout_allowed) {
      return {ExecutionOptimizerPlanAdmissionStatus::parallel_fanout_not_allowed};
    }
    if (ExecutionProducerHasSideEffects(producer)) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::side_effecting_parallel_refused};
    }
    if (!uses_stable_replay && !ExecutionProducerIsRewindable(producer)) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::
              parallel_requires_materialized_or_rewindable};
    }
    if (!uses_stable_replay &&
        !ExecutionProducerIsSnapshotDeterministic(producer)) {
      return {
          ExecutionOptimizerPlanAdmissionStatus::
              parallel_requires_snapshot_determinism};
    }
  }

  return {};
}

inline ExecutionOptimizerProducerDescriptor
ExecutionOptimizerProducerDescriptorFromTableValue(
    const ExecutionTableValue& value) {
  ExecutionOptimizerProducerDescriptor producer;
  producer.optimizer_producer_uuid = value.producer_uuid;
  producer.descriptor_epoch = value.relation_descriptor.descriptor_epoch;
  producer.stable_name = value.relation_descriptor.stable_name;
  producer.producer_kind = ExecutionOptimizerProducerKind::table_value;
  producer.relation_descriptor = value.relation_descriptor;
  producer.owner_transaction_uuid = value.owner_transaction_uuid;
  producer.snapshot_uuid = value.snapshot_uuid;
  producer.security_policy_uuid = value.security_policy_uuid;
  producer.rewindability = value.rewindable
                               ? ExecutionProducerRewindability::rewindable
                               : ExecutionProducerRewindability::forward_only;
  producer.determinism =
      value.deterministic
          ? ExecutionProducerDeterminism::snapshot_deterministic
          : ExecutionProducerDeterminism::nondeterministic;
  producer.side_effect_profile =
      value.side_effecting
          ? ExecutionProducerSideEffectProfile::side_effecting
          : ExecutionProducerSideEffectProfile::side_effect_free;
  return producer;
}

// SEARCH_KEY: EDR_EXECUTION_DISTRIBUTED_HANDLE_DESCRIPTOR
enum class ExecutionDistributedHandleKind : std::uint8_t {
  distributed_cursor = 0,
  distributed_table_value = 1,
  remote_result_channel = 2
};

constexpr bool ExecutionDistributedHandleKindIsValid(
    ExecutionDistributedHandleKind handle_kind) noexcept {
  switch (handle_kind) {
    case ExecutionDistributedHandleKind::distributed_cursor:
    case ExecutionDistributedHandleKind::distributed_table_value:
    case ExecutionDistributedHandleKind::remote_result_channel:
      return true;
  }
  return false;
}

struct ExecutionDistributedStreamWindow {
  std::uint32_t max_rows_per_credit = 1;
  std::uint32_t max_bytes_per_credit = 1;
  std::uint32_t granted_row_credits = 1;
  std::uint32_t in_flight_rows = 0;
  std::uint32_t in_flight_bytes = 0;
  std::uint64_t next_sequence = 0;
  std::uint64_t acknowledged_sequence = 0;
  std::uint64_t final_sequence = 0;
  bool final_sequence_known = false;
};

struct ExecutionDistributedHandleDescriptor {
  Uuid distributed_handle_uuid{};
  Uuid coordinator_handle_uuid{};
  Uuid worker_handle_uuid{};
  Uuid coordinator_node_uuid{};
  Uuid worker_node_uuid{};
  Uuid owner_session_uuid{};
  Uuid owner_transaction_uuid{};
  Uuid snapshot_uuid{};
  Uuid security_policy_uuid{};
  std::uint64_t coordinator_epoch = 0;
  std::uint64_t worker_epoch = 0;
  ExecutionDistributedHandleKind handle_kind =
      ExecutionDistributedHandleKind::distributed_cursor;
  ExecutionRelationDescriptor remote_fragment_descriptor;
  PortalState portal_state = PortalState::declared;
  ExecutionDistributedStreamWindow stream_window;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool cancellation_requested = false;
  bool cancellation_acknowledged = false;
  Uuid cancellation_uuid{};
  bool cleanup_required = false;
  Uuid cleanup_owner_uuid{};
};

enum class ExecutionDistributedHandleStatus : std::uint8_t {
  ok = 0,
  distributed_handle_uuid_required = 1,
  coordinator_handle_uuid_required = 2,
  worker_handle_uuid_required = 3,
  coordinator_worker_handle_collision = 4,
  coordinator_node_uuid_required = 5,
  worker_node_uuid_required = 6,
  owner_session_uuid_required = 7,
  owner_transaction_uuid_required = 8,
  snapshot_uuid_required = 9,
  security_policy_uuid_required = 10,
  coordinator_epoch_required = 11,
  worker_epoch_required = 12,
  descriptor_not_authoritative = 13,
  descriptor_parser_dependent = 14,
  handle_kind_invalid = 15,
  relation_descriptor_invalid = 16,
  relation_kind_invalid = 17,
  portal_state_invalid = 18,
  stream_row_credit_required = 19,
  stream_byte_credit_required = 20,
  in_flight_rows_exceed_window = 21,
  in_flight_bytes_exceed_window = 22,
  acknowledged_sequence_ahead = 23,
  final_sequence_before_ack = 24,
  cancellation_uuid_required = 25,
  cancellation_ack_without_request = 26,
  cleanup_owner_required = 27,
  cleanup_state_required = 28,
  cleanup_required_for_state = 29
};

constexpr std::string_view ExecutionDistributedHandleStatusName(
    ExecutionDistributedHandleStatus status) noexcept {
  switch (status) {
    case ExecutionDistributedHandleStatus::ok:
      return "ok";
    case ExecutionDistributedHandleStatus::distributed_handle_uuid_required:
      return "distributed_handle_uuid_required";
    case ExecutionDistributedHandleStatus::coordinator_handle_uuid_required:
      return "coordinator_handle_uuid_required";
    case ExecutionDistributedHandleStatus::worker_handle_uuid_required:
      return "worker_handle_uuid_required";
    case ExecutionDistributedHandleStatus::coordinator_worker_handle_collision:
      return "coordinator_worker_handle_collision";
    case ExecutionDistributedHandleStatus::coordinator_node_uuid_required:
      return "coordinator_node_uuid_required";
    case ExecutionDistributedHandleStatus::worker_node_uuid_required:
      return "worker_node_uuid_required";
    case ExecutionDistributedHandleStatus::owner_session_uuid_required:
      return "owner_session_uuid_required";
    case ExecutionDistributedHandleStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionDistributedHandleStatus::snapshot_uuid_required:
      return "snapshot_uuid_required";
    case ExecutionDistributedHandleStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionDistributedHandleStatus::coordinator_epoch_required:
      return "coordinator_epoch_required";
    case ExecutionDistributedHandleStatus::worker_epoch_required:
      return "worker_epoch_required";
    case ExecutionDistributedHandleStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionDistributedHandleStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionDistributedHandleStatus::handle_kind_invalid:
      return "handle_kind_invalid";
    case ExecutionDistributedHandleStatus::relation_descriptor_invalid:
      return "relation_descriptor_invalid";
    case ExecutionDistributedHandleStatus::relation_kind_invalid:
      return "relation_kind_invalid";
    case ExecutionDistributedHandleStatus::portal_state_invalid:
      return "portal_state_invalid";
    case ExecutionDistributedHandleStatus::stream_row_credit_required:
      return "stream_row_credit_required";
    case ExecutionDistributedHandleStatus::stream_byte_credit_required:
      return "stream_byte_credit_required";
    case ExecutionDistributedHandleStatus::in_flight_rows_exceed_window:
      return "in_flight_rows_exceed_window";
    case ExecutionDistributedHandleStatus::in_flight_bytes_exceed_window:
      return "in_flight_bytes_exceed_window";
    case ExecutionDistributedHandleStatus::acknowledged_sequence_ahead:
      return "acknowledged_sequence_ahead";
    case ExecutionDistributedHandleStatus::final_sequence_before_ack:
      return "final_sequence_before_ack";
    case ExecutionDistributedHandleStatus::cancellation_uuid_required:
      return "cancellation_uuid_required";
    case ExecutionDistributedHandleStatus::cancellation_ack_without_request:
      return "cancellation_ack_without_request";
    case ExecutionDistributedHandleStatus::cleanup_owner_required:
      return "cleanup_owner_required";
    case ExecutionDistributedHandleStatus::cleanup_state_required:
      return "cleanup_state_required";
    case ExecutionDistributedHandleStatus::cleanup_required_for_state:
      return "cleanup_required_for_state";
  }
  return "unknown_status";
}

struct ExecutionDistributedHandleValidationResult {
  ExecutionDistributedHandleStatus status =
      ExecutionDistributedHandleStatus::ok;
  ExecutionRelationDescriptorStatus relation_status =
      ExecutionRelationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionDistributedHandleStatus::ok;
  }
};

constexpr bool ExecutionDistributedHandleCleanupStateAllowed(
    PortalState state) noexcept {
  return state == PortalState::cleanup_pending || state == PortalState::error ||
         state == PortalState::closed;
}

inline ExecutionDistributedHandleValidationResult
ValidateExecutionDistributedHandleDescriptor(
    const ExecutionDistributedHandleDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.distributed_handle_uuid)) {
    return {ExecutionDistributedHandleStatus::
                distributed_handle_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.coordinator_handle_uuid)) {
    return {ExecutionDistributedHandleStatus::
                coordinator_handle_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.worker_handle_uuid)) {
    return {ExecutionDistributedHandleStatus::worker_handle_uuid_required};
  }
  if (ExecutionDataPacketUuidEquals(descriptor.coordinator_handle_uuid,
                                    descriptor.worker_handle_uuid)) {
    return {
        ExecutionDistributedHandleStatus::coordinator_worker_handle_collision};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.coordinator_node_uuid)) {
    return {ExecutionDistributedHandleStatus::coordinator_node_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.worker_node_uuid)) {
    return {ExecutionDistributedHandleStatus::worker_node_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.owner_session_uuid)) {
    return {ExecutionDistributedHandleStatus::owner_session_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.owner_transaction_uuid)) {
    return {ExecutionDistributedHandleStatus::owner_transaction_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.snapshot_uuid)) {
    return {ExecutionDistributedHandleStatus::snapshot_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.security_policy_uuid)) {
    return {ExecutionDistributedHandleStatus::security_policy_uuid_required};
  }
  if (descriptor.coordinator_epoch == 0) {
    return {ExecutionDistributedHandleStatus::coordinator_epoch_required};
  }
  if (descriptor.worker_epoch == 0) {
    return {ExecutionDistributedHandleStatus::worker_epoch_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionDistributedHandleStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionDistributedHandleStatus::descriptor_parser_dependent};
  }
  if (!ExecutionDistributedHandleKindIsValid(descriptor.handle_kind)) {
    return {ExecutionDistributedHandleStatus::handle_kind_invalid};
  }
  const auto relation_result =
      ValidateExecutionRelationDescriptor(descriptor.remote_fragment_descriptor);
  if (!relation_result.ok()) {
    return {ExecutionDistributedHandleStatus::relation_descriptor_invalid,
            relation_result.status};
  }
  if (descriptor.remote_fragment_descriptor.relation_kind !=
      ExecutionRelationKind::remote_fragment) {
    return {ExecutionDistributedHandleStatus::relation_kind_invalid};
  }
  if (!PortalStateIsValid(descriptor.portal_state)) {
    return {ExecutionDistributedHandleStatus::portal_state_invalid};
  }
  if (descriptor.stream_window.max_rows_per_credit == 0 ||
      descriptor.stream_window.granted_row_credits == 0) {
    return {ExecutionDistributedHandleStatus::stream_row_credit_required};
  }
  if (descriptor.stream_window.max_bytes_per_credit == 0) {
    return {ExecutionDistributedHandleStatus::stream_byte_credit_required};
  }
  const auto max_window_rows =
      descriptor.stream_window.max_rows_per_credit *
      descriptor.stream_window.granted_row_credits;
  const auto max_window_bytes =
      descriptor.stream_window.max_bytes_per_credit *
      descriptor.stream_window.granted_row_credits;
  if (descriptor.stream_window.in_flight_rows > max_window_rows) {
    return {ExecutionDistributedHandleStatus::in_flight_rows_exceed_window};
  }
  if (descriptor.stream_window.in_flight_bytes > max_window_bytes) {
    return {ExecutionDistributedHandleStatus::in_flight_bytes_exceed_window};
  }
  if (descriptor.stream_window.acknowledged_sequence >
      descriptor.stream_window.next_sequence) {
    return {ExecutionDistributedHandleStatus::acknowledged_sequence_ahead};
  }
  if (descriptor.stream_window.final_sequence_known &&
      descriptor.stream_window.final_sequence <
          descriptor.stream_window.acknowledged_sequence) {
    return {ExecutionDistributedHandleStatus::final_sequence_before_ack};
  }
  if (descriptor.cancellation_requested &&
      ExecutionDataPacketUuidIsNil(descriptor.cancellation_uuid)) {
    return {ExecutionDistributedHandleStatus::cancellation_uuid_required};
  }
  if (descriptor.cancellation_acknowledged &&
      !descriptor.cancellation_requested) {
    return {ExecutionDistributedHandleStatus::cancellation_ack_without_request};
  }
  if (descriptor.cleanup_required &&
      ExecutionDataPacketUuidIsNil(descriptor.cleanup_owner_uuid)) {
    return {ExecutionDistributedHandleStatus::cleanup_owner_required};
  }
  if (descriptor.cleanup_required &&
      !ExecutionDistributedHandleCleanupStateAllowed(descriptor.portal_state)) {
    return {ExecutionDistributedHandleStatus::cleanup_state_required};
  }
  if (descriptor.portal_state == PortalState::cleanup_pending &&
      !descriptor.cleanup_required) {
    return {ExecutionDistributedHandleStatus::cleanup_required_for_state};
  }
  return {};
}

inline bool ExecutionTypeDescriptorHasModifierFlag(
    const ExecutionTypeDescriptor& descriptor,
    ExecutionTypeModifierFlag flag) noexcept {
  return (descriptor.modifier_flags & ExecutionTypeModifierFlagBit(flag)) != 0;
}

// SEARCH_KEY: EDR_EXECUTION_DOMAIN_DESCRIPTOR
inline constexpr std::size_t kExecutionDomainStackMaxDepth = 32;
inline constexpr std::size_t kExecutionDomainConstraintMaxCount = 1024;

enum class ExecutionDomainKind : std::uint8_t {
  scalar_alias = 0,
  constrained_scalar = 1,
  composite = 2,
  container = 3,
  opaque = 4,
  reference_compatibility = 5
};

enum class ExecutionDomainDefaultPolicy : std::uint8_t {
  none = 0,
  literal = 1,
  expression = 2,
  generated = 3
};

enum class ExecutionDomainConstraintKind : std::uint8_t {
  check = 0,
  not_null = 1,
  range = 2,
  set_membership = 3,
  format = 4,
  custom = 5
};

enum class ExecutionDomainSecurityPolicy : std::uint8_t {
  policy_bound = 0,
  visibility_bound = 1,
  encrypted = 2
};

enum class ExecutionDomainMaskingPolicy : std::uint8_t {
  none = 0,
  null_value = 1,
  fixed_text = 2,
  redacted = 3,
  reveal_last4 = 4
};

enum class ExecutionDomainStorageCodec : std::uint8_t {
  base_descriptor = 0,
  canonical_binary = 1,
  compressed = 2,
  encrypted = 3,
  external_locator = 4,
  reference_native = 5
};

enum class ExecutionDomainCastPolicy : std::uint8_t {
  explicit_only = 0,
  implicit_safe = 1,
  reference_compatibility = 2,
  forbidden = 3
};

enum class ExecutionDomainOperationPolicy : std::uint8_t {
  inherited = 0,
  domain_methods = 1,
  udr_hooks = 2,
  disabled = 3
};

enum class ExecutionDomainElementAddressingPolicy : std::uint8_t {
  none = 0,
  scalar_value = 1,
  canonical_path = 2,
  opaque_accessor = 3,
  reserved_document_pointer = 4
};

constexpr bool ExecutionDomainKindIsValid(
    ExecutionDomainKind kind) noexcept {
  switch (kind) {
    case ExecutionDomainKind::scalar_alias:
    case ExecutionDomainKind::constrained_scalar:
    case ExecutionDomainKind::composite:
    case ExecutionDomainKind::container:
    case ExecutionDomainKind::opaque:
    case ExecutionDomainKind::reference_compatibility:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainDefaultPolicyIsValid(
    ExecutionDomainDefaultPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainDefaultPolicy::none:
    case ExecutionDomainDefaultPolicy::literal:
    case ExecutionDomainDefaultPolicy::expression:
    case ExecutionDomainDefaultPolicy::generated:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainConstraintKindIsValid(
    ExecutionDomainConstraintKind kind) noexcept {
  switch (kind) {
    case ExecutionDomainConstraintKind::check:
    case ExecutionDomainConstraintKind::not_null:
    case ExecutionDomainConstraintKind::range:
    case ExecutionDomainConstraintKind::set_membership:
    case ExecutionDomainConstraintKind::format:
    case ExecutionDomainConstraintKind::custom:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainSecurityPolicyIsValid(
    ExecutionDomainSecurityPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainSecurityPolicy::policy_bound:
    case ExecutionDomainSecurityPolicy::visibility_bound:
    case ExecutionDomainSecurityPolicy::encrypted:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainMaskingPolicyIsValid(
    ExecutionDomainMaskingPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainMaskingPolicy::none:
    case ExecutionDomainMaskingPolicy::null_value:
    case ExecutionDomainMaskingPolicy::fixed_text:
    case ExecutionDomainMaskingPolicy::redacted:
    case ExecutionDomainMaskingPolicy::reveal_last4:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainStorageCodecIsValid(
    ExecutionDomainStorageCodec codec) noexcept {
  switch (codec) {
    case ExecutionDomainStorageCodec::base_descriptor:
    case ExecutionDomainStorageCodec::canonical_binary:
    case ExecutionDomainStorageCodec::compressed:
    case ExecutionDomainStorageCodec::encrypted:
    case ExecutionDomainStorageCodec::external_locator:
    case ExecutionDomainStorageCodec::reference_native:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainCastPolicyIsValid(
    ExecutionDomainCastPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainCastPolicy::explicit_only:
    case ExecutionDomainCastPolicy::implicit_safe:
    case ExecutionDomainCastPolicy::reference_compatibility:
    case ExecutionDomainCastPolicy::forbidden:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainOperationPolicyIsValid(
    ExecutionDomainOperationPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainOperationPolicy::inherited:
    case ExecutionDomainOperationPolicy::domain_methods:
    case ExecutionDomainOperationPolicy::udr_hooks:
    case ExecutionDomainOperationPolicy::disabled:
      return true;
  }
  return false;
}

constexpr bool ExecutionDomainElementAddressingPolicyIsValid(
    ExecutionDomainElementAddressingPolicy policy) noexcept {
  switch (policy) {
    case ExecutionDomainElementAddressingPolicy::none:
    case ExecutionDomainElementAddressingPolicy::scalar_value:
    case ExecutionDomainElementAddressingPolicy::canonical_path:
    case ExecutionDomainElementAddressingPolicy::opaque_accessor:
    case ExecutionDomainElementAddressingPolicy::reserved_document_pointer:
      return true;
  }
  return false;
}

struct ExecutionDomainConstraintDescriptor {
  Uuid constraint_uuid{};
  std::uint64_t constraint_epoch = 0;
  ExecutionDomainConstraintKind constraint_kind =
      ExecutionDomainConstraintKind::check;
  std::string stable_name;
  bool canonical_envelope_present = true;
  bool enforced = true;
  bool parser_independent = true;
};

struct ExecutionDomainReferenceMetadata {
  bool present = false;
  Uuid reference_profile_uuid{};
  Uuid reference_mapping_uuid{};
  std::string reference_family;
  std::string reference_type_name;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct ExecutionDomainDescriptor {
  Uuid domain_uuid{};
  Uuid domain_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::uint64_t domain_version = 1;
  std::string stable_name;
  ExecutionDomainKind domain_kind = ExecutionDomainKind::scalar_alias;
  ExecutionTypeDescriptor value_descriptor;
  ExecutionTypeDescriptor storage_descriptor;
  std::vector<Uuid> domain_stack;
  ExecutionDomainDefaultPolicy default_policy =
      ExecutionDomainDefaultPolicy::none;
  ExecutionValueState default_value_state =
      ExecutionValueState::default_requested;
  std::vector<std::uint8_t> default_payload;
  Uuid default_expression_uuid{};
  std::vector<ExecutionDomainConstraintDescriptor> constraints;
  ExecutionDomainSecurityPolicy security_policy =
      ExecutionDomainSecurityPolicy::policy_bound;
  Uuid security_policy_uuid{};
  std::uint64_t security_policy_epoch = 0;
  ExecutionDomainMaskingPolicy masking_policy =
      ExecutionDomainMaskingPolicy::none;
  Uuid masking_policy_uuid{};
  std::uint64_t masking_policy_epoch = 0;
  ExecutionDomainStorageCodec storage_codec =
      ExecutionDomainStorageCodec::base_descriptor;
  Uuid storage_codec_uuid{};
  std::uint64_t storage_codec_epoch = 0;
  ExecutionDomainCastPolicy cast_policy =
      ExecutionDomainCastPolicy::explicit_only;
  Uuid cast_policy_uuid{};
  std::uint64_t cast_policy_epoch = 0;
  ExecutionDomainOperationPolicy operation_policy =
      ExecutionDomainOperationPolicy::inherited;
  Uuid operation_policy_uuid{};
  std::uint64_t operation_policy_epoch = 0;
  ExecutionDomainElementAddressingPolicy element_addressing_policy =
      ExecutionDomainElementAddressingPolicy::scalar_value;
  Uuid element_addressing_policy_uuid{};
  std::uint64_t element_addressing_policy_epoch = 0;
  ExecutionDomainReferenceMetadata reference_metadata;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionDomainDescriptorStatus : std::uint8_t {
  ok = 0,
  domain_uuid_required = 1,
  domain_descriptor_uuid_required = 2,
  descriptor_epoch_required = 3,
  domain_version_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  domain_kind_invalid = 8,
  value_descriptor_invalid = 9,
  value_descriptor_domain_flag_required = 10,
  value_descriptor_domain_uuid_mismatch = 11,
  storage_descriptor_invalid = 12,
  domain_stack_required = 13,
  domain_stack_too_deep = 14,
  domain_stack_entry_required = 15,
  domain_stack_current_uuid_mismatch = 16,
  domain_stack_duplicate = 17,
  value_descriptor_domain_stack_flag_required = 18,
  value_descriptor_domain_stack_mismatch = 19,
  storage_descriptor_cycle = 20,
  default_policy_invalid = 21,
  default_literal_state_invalid = 22,
  default_literal_payload_required = 23,
  default_payload_not_allowed = 24,
  default_expression_required = 25,
  default_expression_unexpected = 26,
  default_nullability_violation = 27,
  constraint_required = 28,
  constraint_count_exceeds_limit = 29,
  constraint_uuid_required = 30,
  constraint_epoch_required = 31,
  constraint_kind_invalid = 32,
  constraint_stable_name_required = 33,
  constraint_envelope_required = 34,
  constraint_parser_dependent = 35,
  security_policy_invalid = 36,
  security_policy_uuid_required = 37,
  security_policy_epoch_required = 38,
  masking_policy_invalid = 39,
  masking_policy_uuid_required = 40,
  masking_policy_epoch_required = 41,
  storage_codec_invalid = 42,
  storage_codec_uuid_required = 43,
  storage_codec_epoch_required = 44,
  cast_policy_invalid = 45,
  cast_policy_uuid_required = 46,
  cast_policy_epoch_required = 47,
  operation_policy_invalid = 48,
  operation_policy_uuid_required = 49,
  operation_policy_epoch_required = 50,
  element_addressing_policy_invalid = 51,
  element_addressing_policy_uuid_required = 52,
  element_addressing_policy_epoch_required = 53,
  compound_element_addressing_required = 54,
  opaque_element_addressing_required = 55,
  reference_metadata_required = 56,
  reference_profile_uuid_required = 57,
  reference_mapping_uuid_required = 58,
  reference_family_required = 59,
  reference_type_name_required = 60,
  reference_metadata_not_authoritative = 61,
  reference_metadata_parser_dependent = 62
};

constexpr std::string_view ExecutionDomainDescriptorStatusName(
    ExecutionDomainDescriptorStatus status) noexcept {
  switch (status) {
    case ExecutionDomainDescriptorStatus::ok:
      return "ok";
    case ExecutionDomainDescriptorStatus::domain_uuid_required:
      return "domain_uuid_required";
    case ExecutionDomainDescriptorStatus::domain_descriptor_uuid_required:
      return "domain_descriptor_uuid_required";
    case ExecutionDomainDescriptorStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ExecutionDomainDescriptorStatus::domain_version_required:
      return "domain_version_required";
    case ExecutionDomainDescriptorStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionDomainDescriptorStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionDomainDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionDomainDescriptorStatus::domain_kind_invalid:
      return "domain_kind_invalid";
    case ExecutionDomainDescriptorStatus::value_descriptor_invalid:
      return "value_descriptor_invalid";
    case ExecutionDomainDescriptorStatus::value_descriptor_domain_flag_required:
      return "value_descriptor_domain_flag_required";
    case ExecutionDomainDescriptorStatus::value_descriptor_domain_uuid_mismatch:
      return "value_descriptor_domain_uuid_mismatch";
    case ExecutionDomainDescriptorStatus::storage_descriptor_invalid:
      return "storage_descriptor_invalid";
    case ExecutionDomainDescriptorStatus::domain_stack_required:
      return "domain_stack_required";
    case ExecutionDomainDescriptorStatus::domain_stack_too_deep:
      return "domain_stack_too_deep";
    case ExecutionDomainDescriptorStatus::domain_stack_entry_required:
      return "domain_stack_entry_required";
    case ExecutionDomainDescriptorStatus::domain_stack_current_uuid_mismatch:
      return "domain_stack_current_uuid_mismatch";
    case ExecutionDomainDescriptorStatus::domain_stack_duplicate:
      return "domain_stack_duplicate";
    case ExecutionDomainDescriptorStatus::
        value_descriptor_domain_stack_flag_required:
      return "value_descriptor_domain_stack_flag_required";
    case ExecutionDomainDescriptorStatus::value_descriptor_domain_stack_mismatch:
      return "value_descriptor_domain_stack_mismatch";
    case ExecutionDomainDescriptorStatus::storage_descriptor_cycle:
      return "storage_descriptor_cycle";
    case ExecutionDomainDescriptorStatus::default_policy_invalid:
      return "default_policy_invalid";
    case ExecutionDomainDescriptorStatus::default_literal_state_invalid:
      return "default_literal_state_invalid";
    case ExecutionDomainDescriptorStatus::default_literal_payload_required:
      return "default_literal_payload_required";
    case ExecutionDomainDescriptorStatus::default_payload_not_allowed:
      return "default_payload_not_allowed";
    case ExecutionDomainDescriptorStatus::default_expression_required:
      return "default_expression_required";
    case ExecutionDomainDescriptorStatus::default_expression_unexpected:
      return "default_expression_unexpected";
    case ExecutionDomainDescriptorStatus::default_nullability_violation:
      return "default_nullability_violation";
    case ExecutionDomainDescriptorStatus::constraint_required:
      return "constraint_required";
    case ExecutionDomainDescriptorStatus::constraint_count_exceeds_limit:
      return "constraint_count_exceeds_limit";
    case ExecutionDomainDescriptorStatus::constraint_uuid_required:
      return "constraint_uuid_required";
    case ExecutionDomainDescriptorStatus::constraint_epoch_required:
      return "constraint_epoch_required";
    case ExecutionDomainDescriptorStatus::constraint_kind_invalid:
      return "constraint_kind_invalid";
    case ExecutionDomainDescriptorStatus::constraint_stable_name_required:
      return "constraint_stable_name_required";
    case ExecutionDomainDescriptorStatus::constraint_envelope_required:
      return "constraint_envelope_required";
    case ExecutionDomainDescriptorStatus::constraint_parser_dependent:
      return "constraint_parser_dependent";
    case ExecutionDomainDescriptorStatus::security_policy_invalid:
      return "security_policy_invalid";
    case ExecutionDomainDescriptorStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case ExecutionDomainDescriptorStatus::security_policy_epoch_required:
      return "security_policy_epoch_required";
    case ExecutionDomainDescriptorStatus::masking_policy_invalid:
      return "masking_policy_invalid";
    case ExecutionDomainDescriptorStatus::masking_policy_uuid_required:
      return "masking_policy_uuid_required";
    case ExecutionDomainDescriptorStatus::masking_policy_epoch_required:
      return "masking_policy_epoch_required";
    case ExecutionDomainDescriptorStatus::storage_codec_invalid:
      return "storage_codec_invalid";
    case ExecutionDomainDescriptorStatus::storage_codec_uuid_required:
      return "storage_codec_uuid_required";
    case ExecutionDomainDescriptorStatus::storage_codec_epoch_required:
      return "storage_codec_epoch_required";
    case ExecutionDomainDescriptorStatus::cast_policy_invalid:
      return "cast_policy_invalid";
    case ExecutionDomainDescriptorStatus::cast_policy_uuid_required:
      return "cast_policy_uuid_required";
    case ExecutionDomainDescriptorStatus::cast_policy_epoch_required:
      return "cast_policy_epoch_required";
    case ExecutionDomainDescriptorStatus::operation_policy_invalid:
      return "operation_policy_invalid";
    case ExecutionDomainDescriptorStatus::operation_policy_uuid_required:
      return "operation_policy_uuid_required";
    case ExecutionDomainDescriptorStatus::operation_policy_epoch_required:
      return "operation_policy_epoch_required";
    case ExecutionDomainDescriptorStatus::element_addressing_policy_invalid:
      return "element_addressing_policy_invalid";
    case ExecutionDomainDescriptorStatus::
        element_addressing_policy_uuid_required:
      return "element_addressing_policy_uuid_required";
    case ExecutionDomainDescriptorStatus::
        element_addressing_policy_epoch_required:
      return "element_addressing_policy_epoch_required";
    case ExecutionDomainDescriptorStatus::compound_element_addressing_required:
      return "compound_element_addressing_required";
    case ExecutionDomainDescriptorStatus::opaque_element_addressing_required:
      return "opaque_element_addressing_required";
    case ExecutionDomainDescriptorStatus::reference_metadata_required:
      return "reference_metadata_required";
    case ExecutionDomainDescriptorStatus::reference_profile_uuid_required:
      return "reference_profile_uuid_required";
    case ExecutionDomainDescriptorStatus::reference_mapping_uuid_required:
      return "reference_mapping_uuid_required";
    case ExecutionDomainDescriptorStatus::reference_family_required:
      return "reference_family_required";
    case ExecutionDomainDescriptorStatus::reference_type_name_required:
      return "reference_type_name_required";
    case ExecutionDomainDescriptorStatus::reference_metadata_not_authoritative:
      return "reference_metadata_not_authoritative";
    case ExecutionDomainDescriptorStatus::reference_metadata_parser_dependent:
      return "reference_metadata_parser_dependent";
  }
  return "unknown_status";
}

struct ExecutionDomainDescriptorValidationResult {
  ExecutionDomainDescriptorStatus status =
      ExecutionDomainDescriptorStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t domain_stack_index = 0;
  std::size_t constraint_index = 0;

  bool ok() const noexcept {
    return status == ExecutionDomainDescriptorStatus::ok;
  }
};

inline bool ExecutionDomainUuidVectorEquals(
    const std::vector<Uuid>& left,
    const std::vector<Uuid>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!ExecutionDataPacketUuidEquals(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

inline bool ExecutionDomainStackHasDuplicate(
    const std::vector<Uuid>& domain_stack,
    std::size_t* duplicate_index) noexcept {
  for (std::size_t outer = 0; outer < domain_stack.size(); ++outer) {
    for (std::size_t inner = outer + 1; inner < domain_stack.size(); ++inner) {
      if (ExecutionDataPacketUuidEquals(domain_stack[outer],
                                        domain_stack[inner])) {
        if (duplicate_index != nullptr) {
          *duplicate_index = inner;
        }
        return true;
      }
    }
  }
  return false;
}

inline bool ExecutionDomainKindRequiresConstraint(
    ExecutionDomainKind kind) noexcept {
  return kind == ExecutionDomainKind::constrained_scalar;
}

inline bool ExecutionDomainKindRequiresCompoundAddressing(
    ExecutionDomainKind kind) noexcept {
  return kind == ExecutionDomainKind::composite ||
         kind == ExecutionDomainKind::container;
}

inline ExecutionDomainDescriptorValidationResult
ValidateExecutionDomainDescriptor(
    const ExecutionDomainDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.domain_uuid)) {
    return {ExecutionDomainDescriptorStatus::domain_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.domain_descriptor_uuid)) {
    return {ExecutionDomainDescriptorStatus::domain_descriptor_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::descriptor_epoch_required};
  }
  if (descriptor.domain_version == 0) {
    return {ExecutionDomainDescriptorStatus::domain_version_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ExecutionDomainDescriptorStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionDomainDescriptorStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionDomainDescriptorStatus::descriptor_parser_dependent};
  }
  if (!ExecutionDomainKindIsValid(descriptor.domain_kind)) {
    return {ExecutionDomainDescriptorStatus::domain_kind_invalid};
  }

  const auto value_descriptor_result =
      ValidateExecutionDataPacketDescriptor(descriptor.value_descriptor, 0);
  if (!value_descriptor_result.ok()) {
    return {ExecutionDomainDescriptorStatus::value_descriptor_invalid,
            value_descriptor_result.status};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          descriptor.value_descriptor, ExecutionTypeModifierFlag::domain_uuid)) {
    return {ExecutionDomainDescriptorStatus::
                value_descriptor_domain_flag_required};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.value_descriptor.domain_uuid,
                                    descriptor.domain_uuid)) {
    return {ExecutionDomainDescriptorStatus::
                value_descriptor_domain_uuid_mismatch};
  }

  const auto storage_descriptor_result =
      ValidateExecutionDataPacketDescriptor(descriptor.storage_descriptor, 1);
  if (!storage_descriptor_result.ok()) {
    return {ExecutionDomainDescriptorStatus::storage_descriptor_invalid,
            storage_descriptor_result.status};
  }

  if (descriptor.domain_stack.empty()) {
    return {ExecutionDomainDescriptorStatus::domain_stack_required};
  }
  if (descriptor.domain_stack.size() > kExecutionDomainStackMaxDepth) {
    return {ExecutionDomainDescriptorStatus::domain_stack_too_deep};
  }
  for (std::size_t index = 0; index < descriptor.domain_stack.size(); ++index) {
    if (ExecutionDataPacketUuidIsNil(descriptor.domain_stack[index])) {
      return {ExecutionDomainDescriptorStatus::domain_stack_entry_required,
              ExecutionDataPacketStatus::ok, index};
    }
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.domain_stack.front(),
                                    descriptor.domain_uuid)) {
    return {ExecutionDomainDescriptorStatus::
                domain_stack_current_uuid_mismatch};
  }
  std::size_t duplicate_index = 0;
  if (ExecutionDomainStackHasDuplicate(descriptor.domain_stack,
                                       &duplicate_index)) {
    return {ExecutionDomainDescriptorStatus::domain_stack_duplicate,
            ExecutionDataPacketStatus::ok, duplicate_index};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          descriptor.value_descriptor,
          ExecutionTypeModifierFlag::domain_stack)) {
    return {ExecutionDomainDescriptorStatus::
                value_descriptor_domain_stack_flag_required};
  }
  if (!ExecutionDomainUuidVectorEquals(descriptor.value_descriptor.domain_stack,
                                       descriptor.domain_stack)) {
    return {ExecutionDomainDescriptorStatus::
                value_descriptor_domain_stack_mismatch};
  }
  if (ExecutionTypeDescriptorHasModifierFlag(
          descriptor.storage_descriptor,
          ExecutionTypeModifierFlag::domain_uuid) &&
      ExecutionDataPacketUuidEquals(descriptor.storage_descriptor.domain_uuid,
                                    descriptor.domain_uuid)) {
    return {ExecutionDomainDescriptorStatus::storage_descriptor_cycle};
  }

  if (!ExecutionDomainDefaultPolicyIsValid(descriptor.default_policy)) {
    return {ExecutionDomainDescriptorStatus::default_policy_invalid};
  }
  switch (descriptor.default_policy) {
    case ExecutionDomainDefaultPolicy::none:
      if (!descriptor.default_payload.empty()) {
        return {ExecutionDomainDescriptorStatus::default_payload_not_allowed};
      }
      if (!ExecutionDataPacketUuidIsNil(descriptor.default_expression_uuid)) {
        return {ExecutionDomainDescriptorStatus::default_expression_unexpected};
      }
      break;
    case ExecutionDomainDefaultPolicy::literal:
      if (!PlainValuePayloadStateCodeIsValid(
              static_cast<std::uint8_t>(descriptor.default_value_state))) {
        return {ExecutionDomainDescriptorStatus::default_literal_state_invalid};
      }
      if (descriptor.default_value_state == ExecutionValueState::sql_null &&
          !descriptor.value_descriptor.nullable_allowed) {
        return {ExecutionDomainDescriptorStatus::
                    default_nullability_violation};
      }
      if (ExecutionValueStateHasPayload(descriptor.default_value_state) &&
          descriptor.default_payload.empty()) {
        return {ExecutionDomainDescriptorStatus::
                    default_literal_payload_required};
      }
      if (!ExecutionValueStateHasPayload(descriptor.default_value_state) &&
          !descriptor.default_payload.empty()) {
        return {ExecutionDomainDescriptorStatus::default_payload_not_allowed};
      }
      if (!ExecutionDataPacketUuidIsNil(descriptor.default_expression_uuid)) {
        return {ExecutionDomainDescriptorStatus::default_expression_unexpected};
      }
      break;
    case ExecutionDomainDefaultPolicy::expression:
    case ExecutionDomainDefaultPolicy::generated:
      if (ExecutionDataPacketUuidIsNil(descriptor.default_expression_uuid)) {
        return {ExecutionDomainDescriptorStatus::default_expression_required};
      }
      if (!descriptor.default_payload.empty()) {
        return {ExecutionDomainDescriptorStatus::default_payload_not_allowed};
      }
      break;
  }

  if (ExecutionDomainKindRequiresConstraint(descriptor.domain_kind) &&
      descriptor.constraints.empty()) {
    return {ExecutionDomainDescriptorStatus::constraint_required};
  }
  if (descriptor.constraints.size() > kExecutionDomainConstraintMaxCount) {
    return {ExecutionDomainDescriptorStatus::constraint_count_exceeds_limit};
  }
  for (std::size_t index = 0; index < descriptor.constraints.size(); ++index) {
    const auto& constraint = descriptor.constraints[index];
    if (ExecutionDataPacketUuidIsNil(constraint.constraint_uuid)) {
      return {ExecutionDomainDescriptorStatus::constraint_uuid_required,
              ExecutionDataPacketStatus::ok, 0, index};
    }
    if (constraint.constraint_epoch == 0) {
      return {ExecutionDomainDescriptorStatus::constraint_epoch_required,
              ExecutionDataPacketStatus::ok, 0, index};
    }
    if (!ExecutionDomainConstraintKindIsValid(constraint.constraint_kind)) {
      return {ExecutionDomainDescriptorStatus::constraint_kind_invalid,
              ExecutionDataPacketStatus::ok, 0, index};
    }
    if (constraint.stable_name.empty()) {
      return {ExecutionDomainDescriptorStatus::constraint_stable_name_required,
              ExecutionDataPacketStatus::ok, 0, index};
    }
    if (constraint.enforced && !constraint.canonical_envelope_present) {
      return {ExecutionDomainDescriptorStatus::constraint_envelope_required,
              ExecutionDataPacketStatus::ok, 0, index};
    }
    if (!constraint.parser_independent) {
      return {ExecutionDomainDescriptorStatus::constraint_parser_dependent,
              ExecutionDataPacketStatus::ok, 0, index};
    }
  }

  if (!ExecutionDomainSecurityPolicyIsValid(descriptor.security_policy)) {
    return {ExecutionDomainDescriptorStatus::security_policy_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.security_policy_uuid)) {
    return {ExecutionDomainDescriptorStatus::security_policy_uuid_required};
  }
  if (descriptor.security_policy_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::security_policy_epoch_required};
  }
  if (!ExecutionDomainMaskingPolicyIsValid(descriptor.masking_policy)) {
    return {ExecutionDomainDescriptorStatus::masking_policy_invalid};
  }
  if (descriptor.masking_policy != ExecutionDomainMaskingPolicy::none &&
      ExecutionDataPacketUuidIsNil(descriptor.masking_policy_uuid)) {
    return {ExecutionDomainDescriptorStatus::masking_policy_uuid_required};
  }
  if (descriptor.masking_policy != ExecutionDomainMaskingPolicy::none &&
      descriptor.masking_policy_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::masking_policy_epoch_required};
  }
  if (!ExecutionDomainStorageCodecIsValid(descriptor.storage_codec)) {
    return {ExecutionDomainDescriptorStatus::storage_codec_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.storage_codec_uuid)) {
    return {ExecutionDomainDescriptorStatus::storage_codec_uuid_required};
  }
  if (descriptor.storage_codec_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::storage_codec_epoch_required};
  }
  if (!ExecutionDomainCastPolicyIsValid(descriptor.cast_policy)) {
    return {ExecutionDomainDescriptorStatus::cast_policy_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.cast_policy_uuid)) {
    return {ExecutionDomainDescriptorStatus::cast_policy_uuid_required};
  }
  if (descriptor.cast_policy_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::cast_policy_epoch_required};
  }
  if (!ExecutionDomainOperationPolicyIsValid(descriptor.operation_policy)) {
    return {ExecutionDomainDescriptorStatus::operation_policy_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.operation_policy_uuid)) {
    return {ExecutionDomainDescriptorStatus::operation_policy_uuid_required};
  }
  if (descriptor.operation_policy_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::operation_policy_epoch_required};
  }
  if (!ExecutionDomainElementAddressingPolicyIsValid(
          descriptor.element_addressing_policy)) {
    return {ExecutionDomainDescriptorStatus::
                element_addressing_policy_invalid};
  }
  if (ExecutionDataPacketUuidIsNil(
          descriptor.element_addressing_policy_uuid)) {
    return {ExecutionDomainDescriptorStatus::
                element_addressing_policy_uuid_required};
  }
  if (descriptor.element_addressing_policy_epoch == 0) {
    return {ExecutionDomainDescriptorStatus::
                element_addressing_policy_epoch_required};
  }
  if (ExecutionDomainKindRequiresCompoundAddressing(descriptor.domain_kind) &&
      descriptor.element_addressing_policy !=
          ExecutionDomainElementAddressingPolicy::canonical_path &&
      descriptor.element_addressing_policy !=
          ExecutionDomainElementAddressingPolicy::reserved_document_pointer) {
    return {ExecutionDomainDescriptorStatus::
                compound_element_addressing_required};
  }
  if (descriptor.domain_kind == ExecutionDomainKind::opaque &&
      descriptor.element_addressing_policy !=
          ExecutionDomainElementAddressingPolicy::opaque_accessor) {
    return {ExecutionDomainDescriptorStatus::
                opaque_element_addressing_required};
  }

  if (descriptor.domain_kind == ExecutionDomainKind::reference_compatibility &&
      !descriptor.reference_metadata.present) {
    return {ExecutionDomainDescriptorStatus::reference_metadata_required};
  }
  if (descriptor.reference_metadata.present) {
    if (ExecutionDataPacketUuidIsNil(
            descriptor.reference_metadata.reference_profile_uuid)) {
      return {ExecutionDomainDescriptorStatus::reference_profile_uuid_required};
    }
    if (ExecutionDataPacketUuidIsNil(
            descriptor.reference_metadata.reference_mapping_uuid)) {
      return {ExecutionDomainDescriptorStatus::reference_mapping_uuid_required};
    }
    if (descriptor.reference_metadata.reference_family.empty()) {
      return {ExecutionDomainDescriptorStatus::reference_family_required};
    }
    if (descriptor.reference_metadata.reference_type_name.empty()) {
      return {ExecutionDomainDescriptorStatus::reference_type_name_required};
    }
    if (!descriptor.reference_metadata.descriptor_authoritative) {
      return {ExecutionDomainDescriptorStatus::
                  reference_metadata_not_authoritative};
    }
    if (!descriptor.reference_metadata.parser_independent) {
      return {ExecutionDomainDescriptorStatus::
                  reference_metadata_parser_dependent};
    }
  }

  return {};
}

// SEARCH_KEY: EDR-DOMAIN-ELEMENT-PATH
inline constexpr std::size_t kDomainElementPathMaxSegments = 64;

enum class DomainElementPathSegmentKind : std::uint8_t {
  field_uuid = 0,
  field_ordinal = 1,
  array_index = 2,
  list_index = 3,
  map_key = 4,
  variant_tag = 5,
  range_lower_bound = 6,
  range_upper_bound = 7,
  set_member = 8,
  opaque_accessor = 9,
  reserved_document_pointer = 10
};

constexpr bool DomainElementPathSegmentKindIsValid(
    DomainElementPathSegmentKind kind) noexcept {
  switch (kind) {
    case DomainElementPathSegmentKind::field_uuid:
    case DomainElementPathSegmentKind::field_ordinal:
    case DomainElementPathSegmentKind::array_index:
    case DomainElementPathSegmentKind::list_index:
    case DomainElementPathSegmentKind::map_key:
    case DomainElementPathSegmentKind::variant_tag:
    case DomainElementPathSegmentKind::range_lower_bound:
    case DomainElementPathSegmentKind::range_upper_bound:
    case DomainElementPathSegmentKind::set_member:
    case DomainElementPathSegmentKind::opaque_accessor:
    case DomainElementPathSegmentKind::reserved_document_pointer:
      return true;
  }
  return false;
}

struct DomainElementPathSegment {
  DomainElementPathSegmentKind segment_kind =
      DomainElementPathSegmentKind::field_uuid;
  Uuid element_descriptor_uuid{};
  std::string canonical_token;
  Uuid field_uuid{};
  std::uint32_t field_ordinal = 0;
  bool field_ordinal_present = false;
  std::uint64_t ordinal_index = 0;
  bool ordinal_index_present = false;
  ExecutionTypeDescriptor key_descriptor;
  std::vector<std::uint8_t> key_payload;
  Uuid variant_tag_uuid{};
  Uuid opaque_accessor_uuid{};
  std::string reserved_document_pointer;
};

struct DomainElementPath {
  Uuid path_uuid{};
  Uuid domain_uuid{};
  Uuid path_descriptor_uuid{};
  Uuid element_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string canonical_path;
  ExecutionTypeDescriptor root_descriptor;
  ExecutionTypeDescriptor element_descriptor;
  std::vector<DomainElementPathSegment> segments;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool missing_distinct_from_null = true;
  ExecutionValueState missing_value_state = ExecutionValueState::missing;
  ExecutionValueState null_value_state = ExecutionValueState::sql_null;
};

enum class DomainElementPathStatus : std::uint8_t {
  ok = 0,
  path_uuid_required = 1,
  domain_uuid_required = 2,
  path_descriptor_uuid_required = 3,
  element_descriptor_uuid_required = 4,
  descriptor_epoch_required = 5,
  canonical_path_required = 6,
  descriptor_not_authoritative = 7,
  descriptor_parser_dependent = 8,
  root_descriptor_invalid = 9,
  root_descriptor_domain_flag_required = 10,
  root_descriptor_domain_uuid_mismatch = 11,
  element_descriptor_invalid = 12,
  element_descriptor_uuid_mismatch = 13,
  segments_required = 14,
  segment_count_exceeds_limit = 15,
  segment_kind_invalid = 16,
  segment_canonical_token_required = 17,
  segment_element_descriptor_uuid_required = 18,
  field_uuid_required = 19,
  field_ordinal_required = 20,
  ordinal_index_required = 21,
  map_key_descriptor_invalid = 22,
  map_key_payload_required = 23,
  variant_tag_uuid_required = 24,
  set_member_descriptor_invalid = 25,
  set_member_payload_required = 26,
  opaque_accessor_uuid_required = 27,
  reserved_document_pointer_required = 28,
  final_element_descriptor_uuid_mismatch = 29,
  canonical_path_mismatch = 30,
  missing_value_state_invalid = 31,
  null_value_state_invalid = 32,
  missing_null_not_distinct = 33
};

constexpr std::string_view DomainElementPathStatusName(
    DomainElementPathStatus status) noexcept {
  switch (status) {
    case DomainElementPathStatus::ok:
      return "ok";
    case DomainElementPathStatus::path_uuid_required:
      return "path_uuid_required";
    case DomainElementPathStatus::domain_uuid_required:
      return "domain_uuid_required";
    case DomainElementPathStatus::path_descriptor_uuid_required:
      return "path_descriptor_uuid_required";
    case DomainElementPathStatus::element_descriptor_uuid_required:
      return "element_descriptor_uuid_required";
    case DomainElementPathStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case DomainElementPathStatus::canonical_path_required:
      return "canonical_path_required";
    case DomainElementPathStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case DomainElementPathStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case DomainElementPathStatus::root_descriptor_invalid:
      return "root_descriptor_invalid";
    case DomainElementPathStatus::root_descriptor_domain_flag_required:
      return "root_descriptor_domain_flag_required";
    case DomainElementPathStatus::root_descriptor_domain_uuid_mismatch:
      return "root_descriptor_domain_uuid_mismatch";
    case DomainElementPathStatus::element_descriptor_invalid:
      return "element_descriptor_invalid";
    case DomainElementPathStatus::element_descriptor_uuid_mismatch:
      return "element_descriptor_uuid_mismatch";
    case DomainElementPathStatus::segments_required:
      return "segments_required";
    case DomainElementPathStatus::segment_count_exceeds_limit:
      return "segment_count_exceeds_limit";
    case DomainElementPathStatus::segment_kind_invalid:
      return "segment_kind_invalid";
    case DomainElementPathStatus::segment_canonical_token_required:
      return "segment_canonical_token_required";
    case DomainElementPathStatus::segment_element_descriptor_uuid_required:
      return "segment_element_descriptor_uuid_required";
    case DomainElementPathStatus::field_uuid_required:
      return "field_uuid_required";
    case DomainElementPathStatus::field_ordinal_required:
      return "field_ordinal_required";
    case DomainElementPathStatus::ordinal_index_required:
      return "ordinal_index_required";
    case DomainElementPathStatus::map_key_descriptor_invalid:
      return "map_key_descriptor_invalid";
    case DomainElementPathStatus::map_key_payload_required:
      return "map_key_payload_required";
    case DomainElementPathStatus::variant_tag_uuid_required:
      return "variant_tag_uuid_required";
    case DomainElementPathStatus::set_member_descriptor_invalid:
      return "set_member_descriptor_invalid";
    case DomainElementPathStatus::set_member_payload_required:
      return "set_member_payload_required";
    case DomainElementPathStatus::opaque_accessor_uuid_required:
      return "opaque_accessor_uuid_required";
    case DomainElementPathStatus::reserved_document_pointer_required:
      return "reserved_document_pointer_required";
    case DomainElementPathStatus::final_element_descriptor_uuid_mismatch:
      return "final_element_descriptor_uuid_mismatch";
    case DomainElementPathStatus::canonical_path_mismatch:
      return "canonical_path_mismatch";
    case DomainElementPathStatus::missing_value_state_invalid:
      return "missing_value_state_invalid";
    case DomainElementPathStatus::null_value_state_invalid:
      return "null_value_state_invalid";
    case DomainElementPathStatus::missing_null_not_distinct:
      return "missing_null_not_distinct";
  }
  return "unknown_status";
}

struct DomainElementPathValidationResult {
  DomainElementPathStatus status = DomainElementPathStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t segment_index = 0;

  bool ok() const noexcept {
    return status == DomainElementPathStatus::ok;
  }
};

inline bool DomainElementPathCanonicalPathMatchesSegments(
    const DomainElementPath& path) {
  std::string expected;
  for (const auto& segment : path.segments) {
    expected.push_back('/');
    expected += segment.canonical_token;
  }
  return path.canonical_path == expected;
}

inline DomainElementPathValidationResult ValidateDomainElementPath(
    const DomainElementPath& path) {
  if (ExecutionDataPacketUuidIsNil(path.path_uuid)) {
    return {DomainElementPathStatus::path_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(path.domain_uuid)) {
    return {DomainElementPathStatus::domain_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(path.path_descriptor_uuid)) {
    return {DomainElementPathStatus::path_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(path.element_descriptor_uuid)) {
    return {DomainElementPathStatus::element_descriptor_uuid_required};
  }
  if (path.descriptor_epoch == 0) {
    return {DomainElementPathStatus::descriptor_epoch_required};
  }
  if (path.canonical_path.empty()) {
    return {DomainElementPathStatus::canonical_path_required};
  }
  if (!path.descriptor_authoritative) {
    return {DomainElementPathStatus::descriptor_not_authoritative};
  }
  if (!path.parser_independent) {
    return {DomainElementPathStatus::descriptor_parser_dependent};
  }

  const auto root_result =
      ValidateExecutionDataPacketDescriptor(path.root_descriptor, 0);
  if (!root_result.ok()) {
    return {DomainElementPathStatus::root_descriptor_invalid,
            root_result.status};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          path.root_descriptor, ExecutionTypeModifierFlag::domain_uuid)) {
    return {DomainElementPathStatus::root_descriptor_domain_flag_required};
  }
  if (!ExecutionDataPacketUuidEquals(path.root_descriptor.domain_uuid,
                                    path.domain_uuid)) {
    return {DomainElementPathStatus::root_descriptor_domain_uuid_mismatch};
  }

  const auto element_result =
      ValidateExecutionDataPacketDescriptor(path.element_descriptor, 1);
  if (!element_result.ok()) {
    return {DomainElementPathStatus::element_descriptor_invalid,
            element_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(path.element_descriptor.descriptor_uuid,
                                    path.element_descriptor_uuid)) {
    return {DomainElementPathStatus::element_descriptor_uuid_mismatch};
  }

  if (path.segments.empty()) {
    return {DomainElementPathStatus::segments_required};
  }
  if (path.segments.size() > kDomainElementPathMaxSegments) {
    return {DomainElementPathStatus::segment_count_exceeds_limit};
  }

  for (std::size_t index = 0; index < path.segments.size(); ++index) {
    const auto& segment = path.segments[index];
    if (!DomainElementPathSegmentKindIsValid(segment.segment_kind)) {
      return {DomainElementPathStatus::segment_kind_invalid,
              ExecutionDataPacketStatus::ok, index};
    }
    if (segment.canonical_token.empty()) {
      return {DomainElementPathStatus::segment_canonical_token_required,
              ExecutionDataPacketStatus::ok, index};
    }
    if (ExecutionDataPacketUuidIsNil(segment.element_descriptor_uuid)) {
      return {DomainElementPathStatus::
                  segment_element_descriptor_uuid_required,
              ExecutionDataPacketStatus::ok, index};
    }

    switch (segment.segment_kind) {
      case DomainElementPathSegmentKind::field_uuid:
        if (ExecutionDataPacketUuidIsNil(segment.field_uuid)) {
          return {DomainElementPathStatus::field_uuid_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      case DomainElementPathSegmentKind::field_ordinal:
        if (!segment.field_ordinal_present) {
          return {DomainElementPathStatus::field_ordinal_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      case DomainElementPathSegmentKind::array_index:
      case DomainElementPathSegmentKind::list_index:
        if (!segment.ordinal_index_present) {
          return {DomainElementPathStatus::ordinal_index_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      case DomainElementPathSegmentKind::map_key: {
        const auto key_result =
            ValidateExecutionDataPacketDescriptor(segment.key_descriptor, 2);
        if (!key_result.ok()) {
          return {DomainElementPathStatus::map_key_descriptor_invalid,
                  key_result.status, index};
        }
        if (segment.key_payload.empty()) {
          return {DomainElementPathStatus::map_key_payload_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      }
      case DomainElementPathSegmentKind::variant_tag:
        if (ExecutionDataPacketUuidIsNil(segment.variant_tag_uuid)) {
          return {DomainElementPathStatus::variant_tag_uuid_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      case DomainElementPathSegmentKind::range_lower_bound:
      case DomainElementPathSegmentKind::range_upper_bound:
        break;
      case DomainElementPathSegmentKind::set_member: {
        const auto key_result =
            ValidateExecutionDataPacketDescriptor(segment.key_descriptor, 2);
        if (!key_result.ok()) {
          return {DomainElementPathStatus::set_member_descriptor_invalid,
                  key_result.status, index};
        }
        if (segment.key_payload.empty()) {
          return {DomainElementPathStatus::set_member_payload_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      }
      case DomainElementPathSegmentKind::opaque_accessor:
        if (ExecutionDataPacketUuidIsNil(segment.opaque_accessor_uuid)) {
          return {DomainElementPathStatus::opaque_accessor_uuid_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
      case DomainElementPathSegmentKind::reserved_document_pointer:
        if (segment.reserved_document_pointer.empty()) {
          return {DomainElementPathStatus::
                      reserved_document_pointer_required,
                  ExecutionDataPacketStatus::ok, index};
        }
        break;
    }
  }

  if (!ExecutionDataPacketUuidEquals(path.segments.back().element_descriptor_uuid,
                                    path.element_descriptor_uuid)) {
    return {DomainElementPathStatus::final_element_descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, path.segments.size() - 1};
  }
  if (!DomainElementPathCanonicalPathMatchesSegments(path)) {
    return {DomainElementPathStatus::canonical_path_mismatch};
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(path.missing_value_state)) ||
      path.missing_value_state != ExecutionValueState::missing) {
    return {DomainElementPathStatus::missing_value_state_invalid};
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(path.null_value_state)) ||
      path.null_value_state != ExecutionValueState::sql_null) {
    return {DomainElementPathStatus::null_value_state_invalid};
  }
  if (!path.missing_distinct_from_null ||
      path.missing_value_state == path.null_value_state) {
    return {DomainElementPathStatus::missing_null_not_distinct};
  }

  return {};
}

// SEARCH_KEY: EDR-DOMAIN-CAST-RULE
// SEARCH_KEY: EDR-DOMAIN-OPERATION-DESCRIPTOR
inline constexpr std::uint32_t kDomainDescriptorPolicyMaxCost = 1000000;
inline constexpr std::size_t kDomainOperationMaxOperands = 16;

enum class DomainNullPolicy : std::uint8_t {
  reject = 0,
  propagate_null = 1,
  substitute_default = 2,
  call_descriptor = 3
};

constexpr bool DomainNullPolicyIsValid(DomainNullPolicy policy) noexcept {
  switch (policy) {
    case DomainNullPolicy::reject:
    case DomainNullPolicy::propagate_null:
    case DomainNullPolicy::substitute_default:
    case DomainNullPolicy::call_descriptor:
      return true;
  }
  return false;
}

enum class DomainMissingPolicy : std::uint8_t {
  reject = 0,
  propagate_missing = 1,
  substitute_default = 2,
  call_descriptor = 3
};

constexpr bool DomainMissingPolicyIsValid(DomainMissingPolicy policy) noexcept {
  switch (policy) {
    case DomainMissingPolicy::reject:
    case DomainMissingPolicy::propagate_missing:
    case DomainMissingPolicy::substitute_default:
    case DomainMissingPolicy::call_descriptor:
      return true;
  }
  return false;
}

enum class DomainSecurityPolicy : std::uint8_t {
  none = 0,
  inherit_source = 1,
  inherit_target = 2,
  explicit_policy = 3,
  definer_rights = 4
};

constexpr bool DomainSecurityPolicyIsValid(
    DomainSecurityPolicy policy) noexcept {
  switch (policy) {
    case DomainSecurityPolicy::none:
    case DomainSecurityPolicy::inherit_source:
    case DomainSecurityPolicy::inherit_target:
    case DomainSecurityPolicy::explicit_policy:
    case DomainSecurityPolicy::definer_rights:
      return true;
  }
  return false;
}

constexpr bool DomainSecurityPolicyRequiresUuid(
    DomainSecurityPolicy policy) noexcept {
  return policy == DomainSecurityPolicy::explicit_policy ||
         policy == DomainSecurityPolicy::definer_rights;
}

enum class DomainResourcePolicy : std::uint8_t {
  none = 0,
  inherit_source = 1,
  inherit_target = 2,
  explicit_resource = 3,
  reference_compatibility = 4
};

constexpr bool DomainResourcePolicyIsValid(
    DomainResourcePolicy policy) noexcept {
  switch (policy) {
    case DomainResourcePolicy::none:
    case DomainResourcePolicy::inherit_source:
    case DomainResourcePolicy::inherit_target:
    case DomainResourcePolicy::explicit_resource:
    case DomainResourcePolicy::reference_compatibility:
      return true;
  }
  return false;
}

constexpr bool DomainResourcePolicyRequiresUuid(
    DomainResourcePolicy policy) noexcept {
  return policy == DomainResourcePolicy::explicit_resource ||
         policy == DomainResourcePolicy::reference_compatibility;
}

enum class DomainDeterminism : std::uint8_t {
  immutable = 0,
  stable = 1,
  statement_stable = 2,
  volatile_state = 3
};

constexpr bool DomainDeterminismIsValid(DomainDeterminism determinism) noexcept {
  switch (determinism) {
    case DomainDeterminism::immutable:
    case DomainDeterminism::stable:
    case DomainDeterminism::statement_stable:
    case DomainDeterminism::volatile_state:
      return true;
  }
  return false;
}

constexpr bool DomainDeterminismAllowsIndex(
    DomainDeterminism determinism) noexcept {
  return determinism == DomainDeterminism::immutable ||
         determinism == DomainDeterminism::stable;
}

enum class DomainCostClass : std::uint8_t {
  constant = 0,
  linear = 1,
  external = 2,
  reference = 3,
  user_defined = 4
};

constexpr bool DomainCostClassIsValid(DomainCostClass cost_class) noexcept {
  switch (cost_class) {
    case DomainCostClass::constant:
    case DomainCostClass::linear:
    case DomainCostClass::external:
    case DomainCostClass::reference:
    case DomainCostClass::user_defined:
      return true;
  }
  return false;
}

enum class DomainIndexEligibility : std::uint8_t {
  not_indexable = 0,
  equality = 1,
  ordered = 2,
  range = 3,
  full_text = 4,
  vector = 5
};

constexpr bool DomainIndexEligibilityIsValid(
    DomainIndexEligibility eligibility) noexcept {
  switch (eligibility) {
    case DomainIndexEligibility::not_indexable:
    case DomainIndexEligibility::equality:
    case DomainIndexEligibility::ordered:
    case DomainIndexEligibility::range:
    case DomainIndexEligibility::full_text:
    case DomainIndexEligibility::vector:
      return true;
  }
  return false;
}

enum class DomainDescriptorImplementationKind : std::uint8_t {
  built_in = 0,
  native_sblr = 1,
  cpp_udr = 2,
  reference_native = 3,
  refused = 4
};

constexpr bool DomainDescriptorImplementationKindIsValid(
    DomainDescriptorImplementationKind kind) noexcept {
  switch (kind) {
    case DomainDescriptorImplementationKind::built_in:
    case DomainDescriptorImplementationKind::native_sblr:
    case DomainDescriptorImplementationKind::cpp_udr:
    case DomainDescriptorImplementationKind::reference_native:
    case DomainDescriptorImplementationKind::refused:
      return true;
  }
  return false;
}

struct DomainCppUdrOperationHook {
  bool present = false;
  Uuid library_uuid{};
  Uuid mapping_descriptor_uuid{};
  std::uint64_t mapping_descriptor_epoch = 0;
  std::uint32_t abi_major = 1;
  std::uint32_t abi_minor = 0;
  std::string entrypoint_symbol;
  bool preserves_descriptors = true;
  bool parser_independent = true;
  bool index_safe = false;
};

enum class DomainCastRuleKind : std::uint8_t {
  implicit = 0,
  assignment = 1,
  explicit_only = 2,
  reference_compatibility = 3,
  prohibited = 4
};

constexpr bool DomainCastRuleKindIsValid(DomainCastRuleKind kind) noexcept {
  switch (kind) {
    case DomainCastRuleKind::implicit:
    case DomainCastRuleKind::assignment:
    case DomainCastRuleKind::explicit_only:
    case DomainCastRuleKind::reference_compatibility:
    case DomainCastRuleKind::prohibited:
      return true;
  }
  return false;
}

struct DomainCastRuleDescriptor {
  Uuid cast_rule_uuid{};
  Uuid cast_policy_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::uint64_t cast_policy_epoch = 0;
  std::string stable_name;
  DomainCastRuleKind cast_kind = DomainCastRuleKind::explicit_only;
  Uuid source_domain_uuid{};
  Uuid target_domain_uuid{};
  Uuid source_descriptor_uuid{};
  Uuid target_descriptor_uuid{};
  ExecutionTypeDescriptor source_descriptor;
  ExecutionTypeDescriptor target_descriptor;
  DomainNullPolicy null_policy = DomainNullPolicy::propagate_null;
  DomainMissingPolicy missing_policy = DomainMissingPolicy::propagate_missing;
  std::vector<std::uint8_t> null_substitution_payload;
  std::vector<std::uint8_t> missing_substitution_payload;
  DomainSecurityPolicy security_policy = DomainSecurityPolicy::none;
  Uuid security_policy_uuid{};
  std::uint64_t security_policy_epoch = 0;
  DomainResourcePolicy collation_policy = DomainResourcePolicy::none;
  Uuid collation_uuid{};
  std::uint64_t collation_epoch = 0;
  DomainResourcePolicy timezone_policy = DomainResourcePolicy::none;
  Uuid timezone_uuid{};
  std::uint64_t timezone_epoch = 0;
  DomainDeterminism determinism = DomainDeterminism::immutable;
  DomainCostClass cost_class = DomainCostClass::constant;
  std::uint32_t estimated_cost = 1;
  DomainIndexEligibility index_eligibility =
      DomainIndexEligibility::not_indexable;
  DomainDescriptorImplementationKind implementation_kind =
      DomainDescriptorImplementationKind::built_in;
  DomainCppUdrOperationHook cpp_udr_hook;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class DomainCastRuleStatus : std::uint8_t {
  ok = 0,
  cast_rule_uuid_required = 1,
  cast_policy_uuid_required = 2,
  descriptor_epoch_required = 3,
  cast_policy_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  source_domain_uuid_required = 8,
  target_domain_uuid_required = 9,
  source_descriptor_uuid_required = 10,
  target_descriptor_uuid_required = 11,
  source_descriptor_invalid = 12,
  target_descriptor_invalid = 13,
  source_descriptor_uuid_mismatch = 14,
  target_descriptor_uuid_mismatch = 15,
  source_descriptor_domain_flag_required = 16,
  target_descriptor_domain_flag_required = 17,
  source_descriptor_domain_uuid_mismatch = 18,
  target_descriptor_domain_uuid_mismatch = 19,
  cast_kind_invalid = 20,
  null_policy_invalid = 21,
  missing_policy_invalid = 22,
  null_substitution_payload_required = 23,
  missing_substitution_payload_required = 24,
  substitution_payload_not_allowed = 25,
  security_policy_invalid = 26,
  security_policy_uuid_required = 27,
  security_policy_epoch_required = 28,
  collation_policy_invalid = 29,
  collation_resource_required = 30,
  collation_epoch_required = 31,
  timezone_policy_invalid = 32,
  timezone_resource_required = 33,
  timezone_epoch_required = 34,
  determinism_invalid = 35,
  cost_class_invalid = 36,
  estimated_cost_required = 37,
  estimated_cost_exceeds_limit = 38,
  index_eligibility_invalid = 39,
  index_requires_deterministic_rule = 40,
  implementation_kind_invalid = 41,
  cpp_udr_hook_required = 42,
  cpp_udr_hook_not_allowed = 43,
  cpp_udr_library_uuid_required = 44,
  cpp_udr_entrypoint_required = 45,
  cpp_udr_mapping_descriptor_uuid_required = 46,
  cpp_udr_mapping_descriptor_epoch_required = 47,
  cpp_udr_abi_required = 48,
  cpp_udr_descriptor_preservation_required = 49,
  cpp_udr_parser_independent_required = 50,
  cpp_udr_index_safety_required = 51,
  prohibited_cast_requires_refused_implementation = 52,
  refused_implementation_not_allowed = 53
};

constexpr std::string_view DomainCastRuleStatusName(
    DomainCastRuleStatus status) noexcept {
  switch (status) {
    case DomainCastRuleStatus::ok:
      return "ok";
    case DomainCastRuleStatus::cast_rule_uuid_required:
      return "cast_rule_uuid_required";
    case DomainCastRuleStatus::cast_policy_uuid_required:
      return "cast_policy_uuid_required";
    case DomainCastRuleStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case DomainCastRuleStatus::cast_policy_epoch_required:
      return "cast_policy_epoch_required";
    case DomainCastRuleStatus::stable_name_required:
      return "stable_name_required";
    case DomainCastRuleStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case DomainCastRuleStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case DomainCastRuleStatus::source_domain_uuid_required:
      return "source_domain_uuid_required";
    case DomainCastRuleStatus::target_domain_uuid_required:
      return "target_domain_uuid_required";
    case DomainCastRuleStatus::source_descriptor_uuid_required:
      return "source_descriptor_uuid_required";
    case DomainCastRuleStatus::target_descriptor_uuid_required:
      return "target_descriptor_uuid_required";
    case DomainCastRuleStatus::source_descriptor_invalid:
      return "source_descriptor_invalid";
    case DomainCastRuleStatus::target_descriptor_invalid:
      return "target_descriptor_invalid";
    case DomainCastRuleStatus::source_descriptor_uuid_mismatch:
      return "source_descriptor_uuid_mismatch";
    case DomainCastRuleStatus::target_descriptor_uuid_mismatch:
      return "target_descriptor_uuid_mismatch";
    case DomainCastRuleStatus::source_descriptor_domain_flag_required:
      return "source_descriptor_domain_flag_required";
    case DomainCastRuleStatus::target_descriptor_domain_flag_required:
      return "target_descriptor_domain_flag_required";
    case DomainCastRuleStatus::source_descriptor_domain_uuid_mismatch:
      return "source_descriptor_domain_uuid_mismatch";
    case DomainCastRuleStatus::target_descriptor_domain_uuid_mismatch:
      return "target_descriptor_domain_uuid_mismatch";
    case DomainCastRuleStatus::cast_kind_invalid:
      return "cast_kind_invalid";
    case DomainCastRuleStatus::null_policy_invalid:
      return "null_policy_invalid";
    case DomainCastRuleStatus::missing_policy_invalid:
      return "missing_policy_invalid";
    case DomainCastRuleStatus::null_substitution_payload_required:
      return "null_substitution_payload_required";
    case DomainCastRuleStatus::missing_substitution_payload_required:
      return "missing_substitution_payload_required";
    case DomainCastRuleStatus::substitution_payload_not_allowed:
      return "substitution_payload_not_allowed";
    case DomainCastRuleStatus::security_policy_invalid:
      return "security_policy_invalid";
    case DomainCastRuleStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case DomainCastRuleStatus::security_policy_epoch_required:
      return "security_policy_epoch_required";
    case DomainCastRuleStatus::collation_policy_invalid:
      return "collation_policy_invalid";
    case DomainCastRuleStatus::collation_resource_required:
      return "collation_resource_required";
    case DomainCastRuleStatus::collation_epoch_required:
      return "collation_epoch_required";
    case DomainCastRuleStatus::timezone_policy_invalid:
      return "timezone_policy_invalid";
    case DomainCastRuleStatus::timezone_resource_required:
      return "timezone_resource_required";
    case DomainCastRuleStatus::timezone_epoch_required:
      return "timezone_epoch_required";
    case DomainCastRuleStatus::determinism_invalid:
      return "determinism_invalid";
    case DomainCastRuleStatus::cost_class_invalid:
      return "cost_class_invalid";
    case DomainCastRuleStatus::estimated_cost_required:
      return "estimated_cost_required";
    case DomainCastRuleStatus::estimated_cost_exceeds_limit:
      return "estimated_cost_exceeds_limit";
    case DomainCastRuleStatus::index_eligibility_invalid:
      return "index_eligibility_invalid";
    case DomainCastRuleStatus::index_requires_deterministic_rule:
      return "index_requires_deterministic_rule";
    case DomainCastRuleStatus::implementation_kind_invalid:
      return "implementation_kind_invalid";
    case DomainCastRuleStatus::cpp_udr_hook_required:
      return "cpp_udr_hook_required";
    case DomainCastRuleStatus::cpp_udr_hook_not_allowed:
      return "cpp_udr_hook_not_allowed";
    case DomainCastRuleStatus::cpp_udr_library_uuid_required:
      return "cpp_udr_library_uuid_required";
    case DomainCastRuleStatus::cpp_udr_entrypoint_required:
      return "cpp_udr_entrypoint_required";
    case DomainCastRuleStatus::cpp_udr_mapping_descriptor_uuid_required:
      return "cpp_udr_mapping_descriptor_uuid_required";
    case DomainCastRuleStatus::cpp_udr_mapping_descriptor_epoch_required:
      return "cpp_udr_mapping_descriptor_epoch_required";
    case DomainCastRuleStatus::cpp_udr_abi_required:
      return "cpp_udr_abi_required";
    case DomainCastRuleStatus::cpp_udr_descriptor_preservation_required:
      return "cpp_udr_descriptor_preservation_required";
    case DomainCastRuleStatus::cpp_udr_parser_independent_required:
      return "cpp_udr_parser_independent_required";
    case DomainCastRuleStatus::cpp_udr_index_safety_required:
      return "cpp_udr_index_safety_required";
    case DomainCastRuleStatus::prohibited_cast_requires_refused_implementation:
      return "prohibited_cast_requires_refused_implementation";
    case DomainCastRuleStatus::refused_implementation_not_allowed:
      return "refused_implementation_not_allowed";
  }
  return "unknown_status";
}

struct DomainCastRuleValidationResult {
  DomainCastRuleStatus status = DomainCastRuleStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == DomainCastRuleStatus::ok;
  }
};

inline DomainCastRuleValidationResult ValidateDomainCastRuleDescriptor(
    const DomainCastRuleDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.cast_rule_uuid)) {
    return {DomainCastRuleStatus::cast_rule_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.cast_policy_uuid)) {
    return {DomainCastRuleStatus::cast_policy_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {DomainCastRuleStatus::descriptor_epoch_required};
  }
  if (descriptor.cast_policy_epoch == 0) {
    return {DomainCastRuleStatus::cast_policy_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {DomainCastRuleStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {DomainCastRuleStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {DomainCastRuleStatus::descriptor_parser_dependent};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.source_domain_uuid)) {
    return {DomainCastRuleStatus::source_domain_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.target_domain_uuid)) {
    return {DomainCastRuleStatus::target_domain_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.source_descriptor_uuid)) {
    return {DomainCastRuleStatus::source_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.target_descriptor_uuid)) {
    return {DomainCastRuleStatus::target_descriptor_uuid_required};
  }

  const auto source_result =
      ValidateExecutionDataPacketDescriptor(descriptor.source_descriptor, 0);
  if (!source_result.ok()) {
    return {DomainCastRuleStatus::source_descriptor_invalid,
            source_result.status};
  }
  const auto target_result =
      ValidateExecutionDataPacketDescriptor(descriptor.target_descriptor, 1);
  if (!target_result.ok()) {
    return {DomainCastRuleStatus::target_descriptor_invalid,
            target_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.source_descriptor.descriptor_uuid,
          descriptor.source_descriptor_uuid)) {
    return {DomainCastRuleStatus::source_descriptor_uuid_mismatch};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.target_descriptor.descriptor_uuid,
          descriptor.target_descriptor_uuid)) {
    return {DomainCastRuleStatus::target_descriptor_uuid_mismatch};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          descriptor.source_descriptor,
          ExecutionTypeModifierFlag::domain_uuid)) {
    return {DomainCastRuleStatus::source_descriptor_domain_flag_required};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          descriptor.target_descriptor,
          ExecutionTypeModifierFlag::domain_uuid)) {
    return {DomainCastRuleStatus::target_descriptor_domain_flag_required};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.source_descriptor.domain_uuid,
                                    descriptor.source_domain_uuid)) {
    return {DomainCastRuleStatus::source_descriptor_domain_uuid_mismatch};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.target_descriptor.domain_uuid,
                                    descriptor.target_domain_uuid)) {
    return {DomainCastRuleStatus::target_descriptor_domain_uuid_mismatch};
  }

  if (!DomainCastRuleKindIsValid(descriptor.cast_kind)) {
    return {DomainCastRuleStatus::cast_kind_invalid};
  }
  if (!DomainNullPolicyIsValid(descriptor.null_policy)) {
    return {DomainCastRuleStatus::null_policy_invalid};
  }
  if (!DomainMissingPolicyIsValid(descriptor.missing_policy)) {
    return {DomainCastRuleStatus::missing_policy_invalid};
  }
  if (descriptor.null_policy == DomainNullPolicy::substitute_default &&
      descriptor.null_substitution_payload.empty()) {
    return {DomainCastRuleStatus::null_substitution_payload_required};
  }
  if (descriptor.missing_policy == DomainMissingPolicy::substitute_default &&
      descriptor.missing_substitution_payload.empty()) {
    return {DomainCastRuleStatus::missing_substitution_payload_required};
  }
  if (descriptor.null_policy != DomainNullPolicy::substitute_default &&
      !descriptor.null_substitution_payload.empty()) {
    return {DomainCastRuleStatus::substitution_payload_not_allowed};
  }
  if (descriptor.missing_policy != DomainMissingPolicy::substitute_default &&
      !descriptor.missing_substitution_payload.empty()) {
    return {DomainCastRuleStatus::substitution_payload_not_allowed};
  }

  if (!DomainSecurityPolicyIsValid(descriptor.security_policy)) {
    return {DomainCastRuleStatus::security_policy_invalid};
  }
  if (DomainSecurityPolicyRequiresUuid(descriptor.security_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.security_policy_uuid)) {
    return {DomainCastRuleStatus::security_policy_uuid_required};
  }
  if (DomainSecurityPolicyRequiresUuid(descriptor.security_policy) &&
      descriptor.security_policy_epoch == 0) {
    return {DomainCastRuleStatus::security_policy_epoch_required};
  }
  if (!DomainResourcePolicyIsValid(descriptor.collation_policy)) {
    return {DomainCastRuleStatus::collation_policy_invalid};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.collation_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.collation_uuid)) {
    return {DomainCastRuleStatus::collation_resource_required};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.collation_policy) &&
      descriptor.collation_epoch == 0) {
    return {DomainCastRuleStatus::collation_epoch_required};
  }
  if (!DomainResourcePolicyIsValid(descriptor.timezone_policy)) {
    return {DomainCastRuleStatus::timezone_policy_invalid};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.timezone_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.timezone_uuid)) {
    return {DomainCastRuleStatus::timezone_resource_required};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.timezone_policy) &&
      descriptor.timezone_epoch == 0) {
    return {DomainCastRuleStatus::timezone_epoch_required};
  }
  if (!DomainDeterminismIsValid(descriptor.determinism)) {
    return {DomainCastRuleStatus::determinism_invalid};
  }
  if (!DomainCostClassIsValid(descriptor.cost_class)) {
    return {DomainCastRuleStatus::cost_class_invalid};
  }
  if (descriptor.estimated_cost == 0) {
    return {DomainCastRuleStatus::estimated_cost_required};
  }
  if (descriptor.estimated_cost > kDomainDescriptorPolicyMaxCost) {
    return {DomainCastRuleStatus::estimated_cost_exceeds_limit};
  }
  if (!DomainIndexEligibilityIsValid(descriptor.index_eligibility)) {
    return {DomainCastRuleStatus::index_eligibility_invalid};
  }
  if (descriptor.index_eligibility != DomainIndexEligibility::not_indexable &&
      !DomainDeterminismAllowsIndex(descriptor.determinism)) {
    return {DomainCastRuleStatus::index_requires_deterministic_rule};
  }
  if (!DomainDescriptorImplementationKindIsValid(
          descriptor.implementation_kind)) {
    return {DomainCastRuleStatus::implementation_kind_invalid};
  }
  if (descriptor.cast_kind == DomainCastRuleKind::prohibited &&
      descriptor.implementation_kind !=
          DomainDescriptorImplementationKind::refused) {
    return {
        DomainCastRuleStatus::prohibited_cast_requires_refused_implementation};
  }
  if (descriptor.cast_kind != DomainCastRuleKind::prohibited &&
      descriptor.implementation_kind ==
          DomainDescriptorImplementationKind::refused) {
    return {DomainCastRuleStatus::refused_implementation_not_allowed};
  }

  if (descriptor.implementation_kind ==
      DomainDescriptorImplementationKind::cpp_udr) {
    if (!descriptor.cpp_udr_hook.present) {
      return {DomainCastRuleStatus::cpp_udr_hook_required};
    }
    if (ExecutionDataPacketUuidIsNil(
            descriptor.cpp_udr_hook.library_uuid)) {
      return {DomainCastRuleStatus::cpp_udr_library_uuid_required};
    }
    if (descriptor.cpp_udr_hook.entrypoint_symbol.empty()) {
      return {DomainCastRuleStatus::cpp_udr_entrypoint_required};
    }
    if (ExecutionDataPacketUuidIsNil(
            descriptor.cpp_udr_hook.mapping_descriptor_uuid)) {
      return {DomainCastRuleStatus::cpp_udr_mapping_descriptor_uuid_required};
    }
    if (descriptor.cpp_udr_hook.mapping_descriptor_epoch == 0) {
      return {DomainCastRuleStatus::cpp_udr_mapping_descriptor_epoch_required};
    }
    if (descriptor.cpp_udr_hook.abi_major == 0) {
      return {DomainCastRuleStatus::cpp_udr_abi_required};
    }
    if (!descriptor.cpp_udr_hook.preserves_descriptors) {
      return {DomainCastRuleStatus::cpp_udr_descriptor_preservation_required};
    }
    if (!descriptor.cpp_udr_hook.parser_independent) {
      return {DomainCastRuleStatus::cpp_udr_parser_independent_required};
    }
    if (descriptor.index_eligibility != DomainIndexEligibility::not_indexable &&
        !descriptor.cpp_udr_hook.index_safe) {
      return {DomainCastRuleStatus::cpp_udr_index_safety_required};
    }
  } else if (descriptor.cpp_udr_hook.present) {
    return {DomainCastRuleStatus::cpp_udr_hook_not_allowed};
  }

  return {};
}

enum class DomainOperationKind : std::uint8_t {
  comparison = 0,
  arithmetic = 1,
  containment = 2,
  accessor = 3,
  constructor = 4,
  aggregate = 5,
  user_defined = 6
};

constexpr bool DomainOperationKindIsValid(DomainOperationKind kind) noexcept {
  switch (kind) {
    case DomainOperationKind::comparison:
    case DomainOperationKind::arithmetic:
    case DomainOperationKind::containment:
    case DomainOperationKind::accessor:
    case DomainOperationKind::constructor:
    case DomainOperationKind::aggregate:
    case DomainOperationKind::user_defined:
      return true;
  }
  return false;
}

struct DomainOperationOperandDescriptor {
  Uuid operand_descriptor_uuid{};
  Uuid domain_uuid{};
  ExecutionTypeDescriptor descriptor;
  bool required = true;
};

struct DomainOperationDescriptor {
  Uuid operation_uuid{};
  Uuid operation_policy_uuid{};
  Uuid domain_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::uint64_t operation_policy_epoch = 0;
  std::string stable_name;
  DomainOperationKind operation_kind = DomainOperationKind::user_defined;
  std::uint32_t min_arity = 1;
  std::uint32_t max_arity = 1;
  std::vector<DomainOperationOperandDescriptor> operands;
  Uuid result_descriptor_uuid{};
  Uuid result_domain_uuid{};
  ExecutionTypeDescriptor result_descriptor;
  DomainNullPolicy null_policy = DomainNullPolicy::propagate_null;
  DomainMissingPolicy missing_policy = DomainMissingPolicy::propagate_missing;
  std::vector<std::uint8_t> null_substitution_payload;
  std::vector<std::uint8_t> missing_substitution_payload;
  DomainSecurityPolicy security_policy = DomainSecurityPolicy::none;
  Uuid security_policy_uuid{};
  std::uint64_t security_policy_epoch = 0;
  DomainResourcePolicy collation_policy = DomainResourcePolicy::none;
  Uuid collation_uuid{};
  std::uint64_t collation_epoch = 0;
  DomainResourcePolicy timezone_policy = DomainResourcePolicy::none;
  Uuid timezone_uuid{};
  std::uint64_t timezone_epoch = 0;
  DomainDeterminism determinism = DomainDeterminism::immutable;
  DomainCostClass cost_class = DomainCostClass::constant;
  std::uint32_t estimated_cost = 1;
  DomainIndexEligibility index_eligibility =
      DomainIndexEligibility::not_indexable;
  DomainDescriptorImplementationKind implementation_kind =
      DomainDescriptorImplementationKind::built_in;
  DomainCppUdrOperationHook cpp_udr_hook;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool has_side_effects = false;
};

enum class DomainOperationDescriptorStatus : std::uint8_t {
  ok = 0,
  operation_uuid_required = 1,
  operation_policy_uuid_required = 2,
  domain_uuid_required = 3,
  descriptor_epoch_required = 4,
  operation_policy_epoch_required = 5,
  stable_name_required = 6,
  descriptor_not_authoritative = 7,
  descriptor_parser_dependent = 8,
  operation_kind_invalid = 9,
  arity_invalid = 10,
  operand_count_exceeds_limit = 11,
  operand_count_out_of_range = 12,
  operand_descriptor_uuid_required = 13,
  operand_descriptor_invalid = 14,
  operand_descriptor_uuid_mismatch = 15,
  operand_domain_flag_required = 16,
  operand_domain_uuid_mismatch = 17,
  result_descriptor_uuid_required = 18,
  result_domain_uuid_required = 19,
  result_descriptor_invalid = 20,
  result_descriptor_uuid_mismatch = 21,
  result_descriptor_domain_flag_required = 22,
  result_descriptor_domain_uuid_mismatch = 23,
  null_policy_invalid = 24,
  missing_policy_invalid = 25,
  null_substitution_payload_required = 26,
  missing_substitution_payload_required = 27,
  substitution_payload_not_allowed = 28,
  security_policy_invalid = 29,
  security_policy_uuid_required = 30,
  security_policy_epoch_required = 31,
  collation_policy_invalid = 32,
  collation_resource_required = 33,
  collation_epoch_required = 34,
  timezone_policy_invalid = 35,
  timezone_resource_required = 36,
  timezone_epoch_required = 37,
  determinism_invalid = 38,
  cost_class_invalid = 39,
  estimated_cost_required = 40,
  estimated_cost_exceeds_limit = 41,
  index_eligibility_invalid = 42,
  index_requires_deterministic_operation = 43,
  side_effecting_operation_not_indexable = 44,
  implementation_kind_invalid = 45,
  cpp_udr_hook_required = 46,
  cpp_udr_hook_not_allowed = 47,
  cpp_udr_library_uuid_required = 48,
  cpp_udr_entrypoint_required = 49,
  cpp_udr_mapping_descriptor_uuid_required = 50,
  cpp_udr_mapping_descriptor_epoch_required = 51,
  cpp_udr_abi_required = 52,
  cpp_udr_descriptor_preservation_required = 53,
  cpp_udr_parser_independent_required = 54,
  cpp_udr_index_safety_required = 55,
  refused_implementation_not_allowed = 56
};

constexpr std::string_view DomainOperationDescriptorStatusName(
    DomainOperationDescriptorStatus status) noexcept {
  switch (status) {
    case DomainOperationDescriptorStatus::ok:
      return "ok";
    case DomainOperationDescriptorStatus::operation_uuid_required:
      return "operation_uuid_required";
    case DomainOperationDescriptorStatus::operation_policy_uuid_required:
      return "operation_policy_uuid_required";
    case DomainOperationDescriptorStatus::domain_uuid_required:
      return "domain_uuid_required";
    case DomainOperationDescriptorStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case DomainOperationDescriptorStatus::operation_policy_epoch_required:
      return "operation_policy_epoch_required";
    case DomainOperationDescriptorStatus::stable_name_required:
      return "stable_name_required";
    case DomainOperationDescriptorStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case DomainOperationDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case DomainOperationDescriptorStatus::operation_kind_invalid:
      return "operation_kind_invalid";
    case DomainOperationDescriptorStatus::arity_invalid:
      return "arity_invalid";
    case DomainOperationDescriptorStatus::operand_count_exceeds_limit:
      return "operand_count_exceeds_limit";
    case DomainOperationDescriptorStatus::operand_count_out_of_range:
      return "operand_count_out_of_range";
    case DomainOperationDescriptorStatus::operand_descriptor_uuid_required:
      return "operand_descriptor_uuid_required";
    case DomainOperationDescriptorStatus::operand_descriptor_invalid:
      return "operand_descriptor_invalid";
    case DomainOperationDescriptorStatus::operand_descriptor_uuid_mismatch:
      return "operand_descriptor_uuid_mismatch";
    case DomainOperationDescriptorStatus::operand_domain_flag_required:
      return "operand_domain_flag_required";
    case DomainOperationDescriptorStatus::operand_domain_uuid_mismatch:
      return "operand_domain_uuid_mismatch";
    case DomainOperationDescriptorStatus::result_descriptor_uuid_required:
      return "result_descriptor_uuid_required";
    case DomainOperationDescriptorStatus::result_domain_uuid_required:
      return "result_domain_uuid_required";
    case DomainOperationDescriptorStatus::result_descriptor_invalid:
      return "result_descriptor_invalid";
    case DomainOperationDescriptorStatus::result_descriptor_uuid_mismatch:
      return "result_descriptor_uuid_mismatch";
    case DomainOperationDescriptorStatus::result_descriptor_domain_flag_required:
      return "result_descriptor_domain_flag_required";
    case DomainOperationDescriptorStatus::
        result_descriptor_domain_uuid_mismatch:
      return "result_descriptor_domain_uuid_mismatch";
    case DomainOperationDescriptorStatus::null_policy_invalid:
      return "null_policy_invalid";
    case DomainOperationDescriptorStatus::missing_policy_invalid:
      return "missing_policy_invalid";
    case DomainOperationDescriptorStatus::null_substitution_payload_required:
      return "null_substitution_payload_required";
    case DomainOperationDescriptorStatus::missing_substitution_payload_required:
      return "missing_substitution_payload_required";
    case DomainOperationDescriptorStatus::substitution_payload_not_allowed:
      return "substitution_payload_not_allowed";
    case DomainOperationDescriptorStatus::security_policy_invalid:
      return "security_policy_invalid";
    case DomainOperationDescriptorStatus::security_policy_uuid_required:
      return "security_policy_uuid_required";
    case DomainOperationDescriptorStatus::security_policy_epoch_required:
      return "security_policy_epoch_required";
    case DomainOperationDescriptorStatus::collation_policy_invalid:
      return "collation_policy_invalid";
    case DomainOperationDescriptorStatus::collation_resource_required:
      return "collation_resource_required";
    case DomainOperationDescriptorStatus::collation_epoch_required:
      return "collation_epoch_required";
    case DomainOperationDescriptorStatus::timezone_policy_invalid:
      return "timezone_policy_invalid";
    case DomainOperationDescriptorStatus::timezone_resource_required:
      return "timezone_resource_required";
    case DomainOperationDescriptorStatus::timezone_epoch_required:
      return "timezone_epoch_required";
    case DomainOperationDescriptorStatus::determinism_invalid:
      return "determinism_invalid";
    case DomainOperationDescriptorStatus::cost_class_invalid:
      return "cost_class_invalid";
    case DomainOperationDescriptorStatus::estimated_cost_required:
      return "estimated_cost_required";
    case DomainOperationDescriptorStatus::estimated_cost_exceeds_limit:
      return "estimated_cost_exceeds_limit";
    case DomainOperationDescriptorStatus::index_eligibility_invalid:
      return "index_eligibility_invalid";
    case DomainOperationDescriptorStatus::
        index_requires_deterministic_operation:
      return "index_requires_deterministic_operation";
    case DomainOperationDescriptorStatus::side_effecting_operation_not_indexable:
      return "side_effecting_operation_not_indexable";
    case DomainOperationDescriptorStatus::implementation_kind_invalid:
      return "implementation_kind_invalid";
    case DomainOperationDescriptorStatus::cpp_udr_hook_required:
      return "cpp_udr_hook_required";
    case DomainOperationDescriptorStatus::cpp_udr_hook_not_allowed:
      return "cpp_udr_hook_not_allowed";
    case DomainOperationDescriptorStatus::cpp_udr_library_uuid_required:
      return "cpp_udr_library_uuid_required";
    case DomainOperationDescriptorStatus::cpp_udr_entrypoint_required:
      return "cpp_udr_entrypoint_required";
    case DomainOperationDescriptorStatus::
        cpp_udr_mapping_descriptor_uuid_required:
      return "cpp_udr_mapping_descriptor_uuid_required";
    case DomainOperationDescriptorStatus::
        cpp_udr_mapping_descriptor_epoch_required:
      return "cpp_udr_mapping_descriptor_epoch_required";
    case DomainOperationDescriptorStatus::cpp_udr_abi_required:
      return "cpp_udr_abi_required";
    case DomainOperationDescriptorStatus::
        cpp_udr_descriptor_preservation_required:
      return "cpp_udr_descriptor_preservation_required";
    case DomainOperationDescriptorStatus::cpp_udr_parser_independent_required:
      return "cpp_udr_parser_independent_required";
    case DomainOperationDescriptorStatus::cpp_udr_index_safety_required:
      return "cpp_udr_index_safety_required";
    case DomainOperationDescriptorStatus::refused_implementation_not_allowed:
      return "refused_implementation_not_allowed";
  }
  return "unknown_status";
}

struct DomainOperationDescriptorValidationResult {
  DomainOperationDescriptorStatus status =
      DomainOperationDescriptorStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t operand_index = 0;

  bool ok() const noexcept {
    return status == DomainOperationDescriptorStatus::ok;
  }
};

inline DomainOperationDescriptorValidationResult
ValidateDomainOperationDescriptor(
    const DomainOperationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.operation_uuid)) {
    return {DomainOperationDescriptorStatus::operation_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.operation_policy_uuid)) {
    return {DomainOperationDescriptorStatus::operation_policy_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.domain_uuid)) {
    return {DomainOperationDescriptorStatus::domain_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {DomainOperationDescriptorStatus::descriptor_epoch_required};
  }
  if (descriptor.operation_policy_epoch == 0) {
    return {DomainOperationDescriptorStatus::operation_policy_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {DomainOperationDescriptorStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {DomainOperationDescriptorStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {DomainOperationDescriptorStatus::descriptor_parser_dependent};
  }
  if (!DomainOperationKindIsValid(descriptor.operation_kind)) {
    return {DomainOperationDescriptorStatus::operation_kind_invalid};
  }
  if (descriptor.min_arity > descriptor.max_arity ||
      descriptor.max_arity == 0) {
    return {DomainOperationDescriptorStatus::arity_invalid};
  }
  if (descriptor.operands.size() > kDomainOperationMaxOperands) {
    return {DomainOperationDescriptorStatus::operand_count_exceeds_limit};
  }
  if (descriptor.operands.size() < descriptor.min_arity ||
      descriptor.operands.size() > descriptor.max_arity) {
    return {DomainOperationDescriptorStatus::operand_count_out_of_range};
  }

  for (std::size_t index = 0; index < descriptor.operands.size(); ++index) {
    const auto& operand = descriptor.operands[index];
    if (ExecutionDataPacketUuidIsNil(operand.operand_descriptor_uuid)) {
      return {DomainOperationDescriptorStatus::operand_descriptor_uuid_required,
              ExecutionDataPacketStatus::ok, index};
    }
    const auto operand_result =
        ValidateExecutionDataPacketDescriptor(operand.descriptor, index);
    if (!operand_result.ok()) {
      return {DomainOperationDescriptorStatus::operand_descriptor_invalid,
              operand_result.status, index};
    }
    if (!ExecutionDataPacketUuidEquals(operand.descriptor.descriptor_uuid,
                                      operand.operand_descriptor_uuid)) {
      return {DomainOperationDescriptorStatus::
                  operand_descriptor_uuid_mismatch,
              ExecutionDataPacketStatus::ok, index};
    }
    if (!ExecutionDataPacketUuidIsNil(operand.domain_uuid)) {
      if (!ExecutionTypeDescriptorHasModifierFlag(
              operand.descriptor, ExecutionTypeModifierFlag::domain_uuid)) {
        return {DomainOperationDescriptorStatus::operand_domain_flag_required,
                ExecutionDataPacketStatus::ok, index};
      }
      if (!ExecutionDataPacketUuidEquals(operand.descriptor.domain_uuid,
                                        operand.domain_uuid)) {
        return {DomainOperationDescriptorStatus::
                    operand_domain_uuid_mismatch,
                ExecutionDataPacketStatus::ok, index};
      }
    }
  }

  if (ExecutionDataPacketUuidIsNil(descriptor.result_descriptor_uuid)) {
    return {DomainOperationDescriptorStatus::result_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.result_domain_uuid)) {
    return {DomainOperationDescriptorStatus::result_domain_uuid_required};
  }
  const auto result_descriptor_result =
      ValidateExecutionDataPacketDescriptor(descriptor.result_descriptor, 1);
  if (!result_descriptor_result.ok()) {
    return {DomainOperationDescriptorStatus::result_descriptor_invalid,
            result_descriptor_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.result_descriptor.descriptor_uuid,
          descriptor.result_descriptor_uuid)) {
    return {DomainOperationDescriptorStatus::result_descriptor_uuid_mismatch};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          descriptor.result_descriptor,
          ExecutionTypeModifierFlag::domain_uuid)) {
    return {DomainOperationDescriptorStatus::
                result_descriptor_domain_flag_required};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.result_descriptor.domain_uuid,
                                    descriptor.result_domain_uuid)) {
    return {DomainOperationDescriptorStatus::
                result_descriptor_domain_uuid_mismatch};
  }

  if (!DomainNullPolicyIsValid(descriptor.null_policy)) {
    return {DomainOperationDescriptorStatus::null_policy_invalid};
  }
  if (!DomainMissingPolicyIsValid(descriptor.missing_policy)) {
    return {DomainOperationDescriptorStatus::missing_policy_invalid};
  }
  if (descriptor.null_policy == DomainNullPolicy::substitute_default &&
      descriptor.null_substitution_payload.empty()) {
    return {DomainOperationDescriptorStatus::
                null_substitution_payload_required};
  }
  if (descriptor.missing_policy == DomainMissingPolicy::substitute_default &&
      descriptor.missing_substitution_payload.empty()) {
    return {DomainOperationDescriptorStatus::
                missing_substitution_payload_required};
  }
  if (descriptor.null_policy != DomainNullPolicy::substitute_default &&
      !descriptor.null_substitution_payload.empty()) {
    return {DomainOperationDescriptorStatus::substitution_payload_not_allowed};
  }
  if (descriptor.missing_policy != DomainMissingPolicy::substitute_default &&
      !descriptor.missing_substitution_payload.empty()) {
    return {DomainOperationDescriptorStatus::substitution_payload_not_allowed};
  }
  if (!DomainSecurityPolicyIsValid(descriptor.security_policy)) {
    return {DomainOperationDescriptorStatus::security_policy_invalid};
  }
  if (DomainSecurityPolicyRequiresUuid(descriptor.security_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.security_policy_uuid)) {
    return {DomainOperationDescriptorStatus::security_policy_uuid_required};
  }
  if (DomainSecurityPolicyRequiresUuid(descriptor.security_policy) &&
      descriptor.security_policy_epoch == 0) {
    return {DomainOperationDescriptorStatus::security_policy_epoch_required};
  }
  if (!DomainResourcePolicyIsValid(descriptor.collation_policy)) {
    return {DomainOperationDescriptorStatus::collation_policy_invalid};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.collation_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.collation_uuid)) {
    return {DomainOperationDescriptorStatus::collation_resource_required};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.collation_policy) &&
      descriptor.collation_epoch == 0) {
    return {DomainOperationDescriptorStatus::collation_epoch_required};
  }
  if (!DomainResourcePolicyIsValid(descriptor.timezone_policy)) {
    return {DomainOperationDescriptorStatus::timezone_policy_invalid};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.timezone_policy) &&
      ExecutionDataPacketUuidIsNil(descriptor.timezone_uuid)) {
    return {DomainOperationDescriptorStatus::timezone_resource_required};
  }
  if (DomainResourcePolicyRequiresUuid(descriptor.timezone_policy) &&
      descriptor.timezone_epoch == 0) {
    return {DomainOperationDescriptorStatus::timezone_epoch_required};
  }
  if (!DomainDeterminismIsValid(descriptor.determinism)) {
    return {DomainOperationDescriptorStatus::determinism_invalid};
  }
  if (!DomainCostClassIsValid(descriptor.cost_class)) {
    return {DomainOperationDescriptorStatus::cost_class_invalid};
  }
  if (descriptor.estimated_cost == 0) {
    return {DomainOperationDescriptorStatus::estimated_cost_required};
  }
  if (descriptor.estimated_cost > kDomainDescriptorPolicyMaxCost) {
    return {DomainOperationDescriptorStatus::estimated_cost_exceeds_limit};
  }
  if (!DomainIndexEligibilityIsValid(descriptor.index_eligibility)) {
    return {DomainOperationDescriptorStatus::index_eligibility_invalid};
  }
  if (descriptor.index_eligibility != DomainIndexEligibility::not_indexable &&
      !DomainDeterminismAllowsIndex(descriptor.determinism)) {
    return {DomainOperationDescriptorStatus::
                index_requires_deterministic_operation};
  }
  if (descriptor.index_eligibility != DomainIndexEligibility::not_indexable &&
      descriptor.has_side_effects) {
    return {DomainOperationDescriptorStatus::
                side_effecting_operation_not_indexable};
  }
  if (!DomainDescriptorImplementationKindIsValid(
          descriptor.implementation_kind)) {
    return {DomainOperationDescriptorStatus::implementation_kind_invalid};
  }
  if (descriptor.implementation_kind ==
      DomainDescriptorImplementationKind::refused) {
    return {DomainOperationDescriptorStatus::refused_implementation_not_allowed};
  }
  if (descriptor.implementation_kind ==
      DomainDescriptorImplementationKind::cpp_udr) {
    if (!descriptor.cpp_udr_hook.present) {
      return {DomainOperationDescriptorStatus::cpp_udr_hook_required};
    }
    if (ExecutionDataPacketUuidIsNil(
            descriptor.cpp_udr_hook.library_uuid)) {
      return {DomainOperationDescriptorStatus::cpp_udr_library_uuid_required};
    }
    if (descriptor.cpp_udr_hook.entrypoint_symbol.empty()) {
      return {DomainOperationDescriptorStatus::cpp_udr_entrypoint_required};
    }
    if (ExecutionDataPacketUuidIsNil(
            descriptor.cpp_udr_hook.mapping_descriptor_uuid)) {
      return {DomainOperationDescriptorStatus::
                  cpp_udr_mapping_descriptor_uuid_required};
    }
    if (descriptor.cpp_udr_hook.mapping_descriptor_epoch == 0) {
      return {DomainOperationDescriptorStatus::
                  cpp_udr_mapping_descriptor_epoch_required};
    }
    if (descriptor.cpp_udr_hook.abi_major == 0) {
      return {DomainOperationDescriptorStatus::cpp_udr_abi_required};
    }
    if (!descriptor.cpp_udr_hook.preserves_descriptors) {
      return {DomainOperationDescriptorStatus::
                  cpp_udr_descriptor_preservation_required};
    }
    if (!descriptor.cpp_udr_hook.parser_independent) {
      return {DomainOperationDescriptorStatus::
                  cpp_udr_parser_independent_required};
    }
    if (descriptor.index_eligibility != DomainIndexEligibility::not_indexable &&
        !descriptor.cpp_udr_hook.index_safe) {
      return {DomainOperationDescriptorStatus::cpp_udr_index_safety_required};
    }
  } else if (descriptor.cpp_udr_hook.present) {
    return {DomainOperationDescriptorStatus::cpp_udr_hook_not_allowed};
  }

  return {};
}

// SEARCH_KEY: DTC-REFERENCE-TYPE-COVERAGE
// SEARCH_KEY: EDR-REFERENCE-TYPE-CAPABILITY
enum class ReferenceTypeRepresentationClass : std::uint8_t {
  native_descriptor = 0,
  domain_compatibility = 1,
  opaque_bridge = 2,
  external_locator = 3,
  refused = 4,
  deferred = 5
};

constexpr bool ReferenceTypeRepresentationClassIsValid(
    ReferenceTypeRepresentationClass representation_class) noexcept {
  switch (representation_class) {
    case ReferenceTypeRepresentationClass::native_descriptor:
    case ReferenceTypeRepresentationClass::domain_compatibility:
    case ReferenceTypeRepresentationClass::opaque_bridge:
    case ReferenceTypeRepresentationClass::external_locator:
    case ReferenceTypeRepresentationClass::refused:
    case ReferenceTypeRepresentationClass::deferred:
      return true;
  }
  return false;
}

constexpr bool ReferenceTypeRepresentationIsExecutable(
    ReferenceTypeRepresentationClass representation_class) noexcept {
  return representation_class != ReferenceTypeRepresentationClass::refused &&
         representation_class != ReferenceTypeRepresentationClass::deferred;
}

constexpr bool ReferenceTypeRepresentationRequiresDomain(
    ReferenceTypeRepresentationClass representation_class) noexcept {
  return representation_class ==
             ReferenceTypeRepresentationClass::domain_compatibility ||
         representation_class == ReferenceTypeRepresentationClass::opaque_bridge;
}

constexpr bool ReferenceTypeRepresentationRequiresExternalLocatorSafety(
    ReferenceTypeRepresentationClass representation_class) noexcept {
  return representation_class == ReferenceTypeRepresentationClass::external_locator;
}

constexpr bool ReferenceTypeRepresentationRequiresOpaqueLifecycle(
    ReferenceTypeRepresentationClass representation_class) noexcept {
  return representation_class == ReferenceTypeRepresentationClass::opaque_bridge;
}

enum class ExecutionTypeCapabilityState : std::uint8_t {
  supported = 0,
  descriptor_cast = 1,
  reference_runtime = 2,
  cpp_udr_bridge = 3,
  llvm_accelerated = 4,
  refused = 5,
  deferred = 6
};

constexpr bool ExecutionTypeCapabilityStateIsValid(
    ExecutionTypeCapabilityState state) noexcept {
  switch (state) {
    case ExecutionTypeCapabilityState::supported:
    case ExecutionTypeCapabilityState::descriptor_cast:
    case ExecutionTypeCapabilityState::reference_runtime:
    case ExecutionTypeCapabilityState::cpp_udr_bridge:
    case ExecutionTypeCapabilityState::llvm_accelerated:
    case ExecutionTypeCapabilityState::refused:
    case ExecutionTypeCapabilityState::deferred:
      return true;
  }
  return false;
}

constexpr bool ExecutionTypeCapabilityStateIsActive(
    ExecutionTypeCapabilityState state) noexcept {
  return state != ExecutionTypeCapabilityState::refused &&
         state != ExecutionTypeCapabilityState::deferred;
}

constexpr bool ExecutionTypeCapabilityStateRequiresCppUdr(
    ExecutionTypeCapabilityState state) noexcept {
  return state == ExecutionTypeCapabilityState::cpp_udr_bridge ||
         state == ExecutionTypeCapabilityState::llvm_accelerated;
}

constexpr bool ExecutionTypeCapabilityStateRequiresLlvm(
    ExecutionTypeCapabilityState state) noexcept {
  return state == ExecutionTypeCapabilityState::llvm_accelerated;
}

struct ReferenceTypeMappingDescriptor {
  Uuid mapping_uuid{};
  Uuid reference_profile_uuid{};
  std::uint64_t mapping_epoch = 0;
  std::string reference_family;
  std::string reference_type_name;
  ReferenceTypeRepresentationClass representation_class =
      ReferenceTypeRepresentationClass::native_descriptor;
  std::string decision_reason;
  Uuid canonical_descriptor_uuid{};
  ExecutionTypeDescriptor canonical_descriptor;
  bool domain_descriptor_present = false;
  Uuid domain_uuid{};
  ExecutionDomainDescriptor domain_descriptor;
  bool cast_rule_present = false;
  Uuid cast_rule_uuid{};
  DomainCastRuleDescriptor cast_rule_descriptor;
  bool operation_descriptor_present = false;
  Uuid operation_uuid{};
  DomainOperationDescriptor operation_descriptor;
  bool external_locator_safe = false;
  bool opaque_lifecycle_managed = false;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ReferenceTypeMappingDescriptorStatus : std::uint8_t {
  ok = 0,
  mapping_uuid_required = 1,
  reference_profile_uuid_required = 2,
  mapping_epoch_required = 3,
  reference_family_required = 4,
  reference_type_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  representation_class_invalid = 8,
  decision_reason_required = 9,
  inactive_mapping_has_execution_binding = 10,
  canonical_descriptor_uuid_required = 11,
  canonical_descriptor_invalid = 12,
  canonical_descriptor_uuid_mismatch = 13,
  domain_descriptor_required = 14,
  domain_descriptor_not_allowed = 15,
  domain_uuid_required = 16,
  domain_descriptor_invalid = 17,
  domain_uuid_mismatch = 18,
  domain_kind_mismatch = 19,
  domain_reference_metadata_mismatch = 20,
  cast_rule_uuid_required = 21,
  cast_rule_descriptor_invalid = 22,
  cast_rule_uuid_mismatch = 23,
  operation_uuid_required = 24,
  operation_descriptor_invalid = 25,
  operation_uuid_mismatch = 26,
  external_locator_safety_required = 27,
  opaque_lifecycle_required = 28
};

constexpr std::string_view ReferenceTypeMappingDescriptorStatusName(
    ReferenceTypeMappingDescriptorStatus status) noexcept {
  switch (status) {
    case ReferenceTypeMappingDescriptorStatus::ok:
      return "ok";
    case ReferenceTypeMappingDescriptorStatus::mapping_uuid_required:
      return "mapping_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::reference_profile_uuid_required:
      return "reference_profile_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::mapping_epoch_required:
      return "mapping_epoch_required";
    case ReferenceTypeMappingDescriptorStatus::reference_family_required:
      return "reference_family_required";
    case ReferenceTypeMappingDescriptorStatus::reference_type_name_required:
      return "reference_type_name_required";
    case ReferenceTypeMappingDescriptorStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ReferenceTypeMappingDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ReferenceTypeMappingDescriptorStatus::representation_class_invalid:
      return "representation_class_invalid";
    case ReferenceTypeMappingDescriptorStatus::decision_reason_required:
      return "decision_reason_required";
    case ReferenceTypeMappingDescriptorStatus::inactive_mapping_has_execution_binding:
      return "inactive_mapping_has_execution_binding";
    case ReferenceTypeMappingDescriptorStatus::
        canonical_descriptor_uuid_required:
      return "canonical_descriptor_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::canonical_descriptor_invalid:
      return "canonical_descriptor_invalid";
    case ReferenceTypeMappingDescriptorStatus::canonical_descriptor_uuid_mismatch:
      return "canonical_descriptor_uuid_mismatch";
    case ReferenceTypeMappingDescriptorStatus::domain_descriptor_required:
      return "domain_descriptor_required";
    case ReferenceTypeMappingDescriptorStatus::domain_descriptor_not_allowed:
      return "domain_descriptor_not_allowed";
    case ReferenceTypeMappingDescriptorStatus::domain_uuid_required:
      return "domain_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::domain_descriptor_invalid:
      return "domain_descriptor_invalid";
    case ReferenceTypeMappingDescriptorStatus::domain_uuid_mismatch:
      return "domain_uuid_mismatch";
    case ReferenceTypeMappingDescriptorStatus::domain_kind_mismatch:
      return "domain_kind_mismatch";
    case ReferenceTypeMappingDescriptorStatus::domain_reference_metadata_mismatch:
      return "domain_reference_metadata_mismatch";
    case ReferenceTypeMappingDescriptorStatus::cast_rule_uuid_required:
      return "cast_rule_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::cast_rule_descriptor_invalid:
      return "cast_rule_descriptor_invalid";
    case ReferenceTypeMappingDescriptorStatus::cast_rule_uuid_mismatch:
      return "cast_rule_uuid_mismatch";
    case ReferenceTypeMappingDescriptorStatus::operation_uuid_required:
      return "operation_uuid_required";
    case ReferenceTypeMappingDescriptorStatus::operation_descriptor_invalid:
      return "operation_descriptor_invalid";
    case ReferenceTypeMappingDescriptorStatus::operation_uuid_mismatch:
      return "operation_uuid_mismatch";
    case ReferenceTypeMappingDescriptorStatus::external_locator_safety_required:
      return "external_locator_safety_required";
    case ReferenceTypeMappingDescriptorStatus::opaque_lifecycle_required:
      return "opaque_lifecycle_required";
  }
  return "unknown_status";
}

struct ReferenceTypeMappingDescriptorValidationResult {
  ReferenceTypeMappingDescriptorStatus status =
      ReferenceTypeMappingDescriptorStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  ExecutionDomainDescriptorStatus domain_status =
      ExecutionDomainDescriptorStatus::ok;
  DomainCastRuleStatus cast_rule_status = DomainCastRuleStatus::ok;
  DomainOperationDescriptorStatus operation_status =
      DomainOperationDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ReferenceTypeMappingDescriptorStatus::ok;
  }
};

inline bool ReferenceTypeMappingHasExecutionBinding(
    const ReferenceTypeMappingDescriptor& descriptor) noexcept {
  return !ExecutionDataPacketUuidIsNil(descriptor.canonical_descriptor_uuid) ||
         !ExecutionDataPacketUuidIsNil(
             descriptor.canonical_descriptor.descriptor_uuid) ||
         descriptor.domain_descriptor_present || descriptor.cast_rule_present ||
         descriptor.operation_descriptor_present;
}

inline bool ReferenceTypeMappingDomainMetadataMatches(
    const ReferenceTypeMappingDescriptor& descriptor) noexcept {
  const auto& metadata = descriptor.domain_descriptor.reference_metadata;
  return metadata.present &&
         ExecutionDataPacketUuidEquals(metadata.reference_profile_uuid,
                                       descriptor.reference_profile_uuid) &&
         ExecutionDataPacketUuidEquals(metadata.reference_mapping_uuid,
                                       descriptor.mapping_uuid) &&
         metadata.reference_family == descriptor.reference_family &&
         metadata.reference_type_name == descriptor.reference_type_name;
}

inline ReferenceTypeMappingDescriptorValidationResult
ValidateReferenceTypeMappingDescriptor(
    const ReferenceTypeMappingDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.mapping_uuid)) {
    return {ReferenceTypeMappingDescriptorStatus::mapping_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.reference_profile_uuid)) {
    return {ReferenceTypeMappingDescriptorStatus::reference_profile_uuid_required};
  }
  if (descriptor.mapping_epoch == 0) {
    return {ReferenceTypeMappingDescriptorStatus::mapping_epoch_required};
  }
  if (descriptor.reference_family.empty()) {
    return {ReferenceTypeMappingDescriptorStatus::reference_family_required};
  }
  if (descriptor.reference_type_name.empty()) {
    return {ReferenceTypeMappingDescriptorStatus::reference_type_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ReferenceTypeMappingDescriptorStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ReferenceTypeMappingDescriptorStatus::descriptor_parser_dependent};
  }
  if (!ReferenceTypeRepresentationClassIsValid(
          descriptor.representation_class)) {
    return {ReferenceTypeMappingDescriptorStatus::representation_class_invalid};
  }

  if (!ReferenceTypeRepresentationIsExecutable(descriptor.representation_class)) {
    if (descriptor.decision_reason.empty()) {
      return {ReferenceTypeMappingDescriptorStatus::decision_reason_required};
    }
    if (ReferenceTypeMappingHasExecutionBinding(descriptor)) {
      return {
          ReferenceTypeMappingDescriptorStatus::inactive_mapping_has_execution_binding};
    }
    return {};
  }

  if (ExecutionDataPacketUuidIsNil(descriptor.canonical_descriptor_uuid)) {
    return {
        ReferenceTypeMappingDescriptorStatus::canonical_descriptor_uuid_required};
  }
  const auto canonical_result =
      ValidateExecutionDataPacketDescriptor(descriptor.canonical_descriptor, 0);
  if (!canonical_result.ok()) {
    return {ReferenceTypeMappingDescriptorStatus::canonical_descriptor_invalid,
            canonical_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.canonical_descriptor.descriptor_uuid,
          descriptor.canonical_descriptor_uuid)) {
    return {
        ReferenceTypeMappingDescriptorStatus::canonical_descriptor_uuid_mismatch};
  }

  if (ReferenceTypeRepresentationRequiresDomain(
          descriptor.representation_class) &&
      !descriptor.domain_descriptor_present) {
    return {ReferenceTypeMappingDescriptorStatus::domain_descriptor_required};
  }
  if (descriptor.representation_class ==
          ReferenceTypeRepresentationClass::native_descriptor &&
      descriptor.domain_descriptor_present) {
    return {ReferenceTypeMappingDescriptorStatus::domain_descriptor_not_allowed};
  }
  if (descriptor.domain_descriptor_present) {
    if (ExecutionDataPacketUuidIsNil(descriptor.domain_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::domain_uuid_required};
    }
    const auto domain_result =
        ValidateExecutionDomainDescriptor(descriptor.domain_descriptor);
    if (!domain_result.ok()) {
      return {ReferenceTypeMappingDescriptorStatus::domain_descriptor_invalid,
              ExecutionDataPacketStatus::ok, domain_result.status};
    }
    if (!ExecutionDataPacketUuidEquals(descriptor.domain_uuid,
                                      descriptor.domain_descriptor.domain_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::domain_uuid_mismatch};
    }
    if (descriptor.representation_class ==
            ReferenceTypeRepresentationClass::domain_compatibility &&
        descriptor.domain_descriptor.domain_kind !=
            ExecutionDomainKind::reference_compatibility) {
      return {ReferenceTypeMappingDescriptorStatus::domain_kind_mismatch};
    }
    if (descriptor.representation_class ==
            ReferenceTypeRepresentationClass::opaque_bridge &&
        descriptor.domain_descriptor.domain_kind !=
            ExecutionDomainKind::opaque) {
      return {ReferenceTypeMappingDescriptorStatus::domain_kind_mismatch};
    }
    if (descriptor.representation_class ==
            ReferenceTypeRepresentationClass::domain_compatibility &&
        !ReferenceTypeMappingDomainMetadataMatches(descriptor)) {
      return {ReferenceTypeMappingDescriptorStatus::domain_reference_metadata_mismatch};
    }
  }

  if (descriptor.cast_rule_present) {
    if (ExecutionDataPacketUuidIsNil(descriptor.cast_rule_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::cast_rule_uuid_required};
    }
    const auto cast_result =
        ValidateDomainCastRuleDescriptor(descriptor.cast_rule_descriptor);
    if (!cast_result.ok()) {
      return {ReferenceTypeMappingDescriptorStatus::cast_rule_descriptor_invalid,
              cast_result.descriptor_status,
              ExecutionDomainDescriptorStatus::ok, cast_result.status};
    }
    if (!ExecutionDataPacketUuidEquals(
            descriptor.cast_rule_uuid,
            descriptor.cast_rule_descriptor.cast_rule_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::cast_rule_uuid_mismatch};
    }
  }

  if (descriptor.operation_descriptor_present) {
    if (ExecutionDataPacketUuidIsNil(descriptor.operation_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::operation_uuid_required};
    }
    const auto operation_result =
        ValidateDomainOperationDescriptor(descriptor.operation_descriptor);
    if (!operation_result.ok()) {
      return {ReferenceTypeMappingDescriptorStatus::operation_descriptor_invalid,
              operation_result.descriptor_status,
              ExecutionDomainDescriptorStatus::ok, DomainCastRuleStatus::ok,
              operation_result.status};
    }
    if (!ExecutionDataPacketUuidEquals(
            descriptor.operation_uuid,
            descriptor.operation_descriptor.operation_uuid)) {
      return {ReferenceTypeMappingDescriptorStatus::operation_uuid_mismatch};
    }
  }

  if (ReferenceTypeRepresentationRequiresExternalLocatorSafety(
          descriptor.representation_class) &&
      !descriptor.external_locator_safe) {
    return {ReferenceTypeMappingDescriptorStatus::
                external_locator_safety_required};
  }
  if (ReferenceTypeRepresentationRequiresOpaqueLifecycle(
          descriptor.representation_class) &&
      !descriptor.opaque_lifecycle_managed) {
    return {ReferenceTypeMappingDescriptorStatus::opaque_lifecycle_required};
  }

  return {};
}

struct ExecutionTypeCapabilityDescriptor {
  Uuid capability_uuid{};
  Uuid mapping_uuid{};
  Uuid reference_profile_uuid{};
  Uuid canonical_descriptor_uuid{};
  std::uint64_t capability_epoch = 0;
  std::string stable_name;
  ExecutionTypeCapabilityState literal_policy =
      ExecutionTypeCapabilityState::supported;
  ExecutionTypeCapabilityState bind_policy =
      ExecutionTypeCapabilityState::supported;
  ExecutionTypeCapabilityState inference_policy =
      ExecutionTypeCapabilityState::supported;
  ExecutionTypeCapabilityState overload_policy =
      ExecutionTypeCapabilityState::supported;
  ExecutionTypeCapabilityState cast_policy =
      ExecutionTypeCapabilityState::descriptor_cast;
  ExecutionTypeCapabilityState operation_policy =
      ExecutionTypeCapabilityState::supported;
  bool cxx_udr_bridge_available = false;
  Uuid cxx_udr_mapping_descriptor_uuid{};
  std::uint64_t cxx_udr_mapping_descriptor_epoch = 0;
  bool llvm_acceleration_available = false;
  Uuid llvm_acceleration_descriptor_uuid{};
  std::uint64_t llvm_acceleration_descriptor_epoch = 0;
  bool external_locator_allowed = false;
  bool external_locator_policy_safe = false;
  bool opaque_lifecycle_managed = false;
  bool reference_superiority_matrix_entry_present = false;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ExecutionTypeCapabilityDescriptorStatus : std::uint8_t {
  ok = 0,
  capability_uuid_required = 1,
  mapping_uuid_required = 2,
  reference_profile_uuid_required = 3,
  canonical_descriptor_uuid_required = 4,
  capability_epoch_required = 5,
  stable_name_required = 6,
  descriptor_not_authoritative = 7,
  descriptor_parser_dependent = 8,
  mapping_descriptor_invalid = 9,
  mapping_uuid_mismatch = 10,
  reference_profile_uuid_mismatch = 11,
  canonical_descriptor_uuid_mismatch = 12,
  literal_policy_invalid = 13,
  bind_policy_invalid = 14,
  inference_policy_invalid = 15,
  overload_policy_invalid = 16,
  cast_policy_invalid = 17,
  operation_policy_invalid = 18,
  active_capability_requires_supported_mapping = 19,
  cast_capability_requires_cast_rule = 20,
  operation_capability_requires_operation_descriptor = 21,
  cpp_udr_mapping_descriptor_uuid_required = 22,
  cpp_udr_mapping_descriptor_epoch_required = 23,
  llvm_acceleration_requires_cpp_udr_bridge = 24,
  llvm_acceleration_descriptor_uuid_required = 25,
  llvm_acceleration_descriptor_epoch_required = 26,
  external_locator_policy_required = 27,
  opaque_lifecycle_required = 28,
  reference_superiority_matrix_required = 29
};

constexpr std::string_view ExecutionTypeCapabilityDescriptorStatusName(
    ExecutionTypeCapabilityDescriptorStatus status) noexcept {
  switch (status) {
    case ExecutionTypeCapabilityDescriptorStatus::ok:
      return "ok";
    case ExecutionTypeCapabilityDescriptorStatus::capability_uuid_required:
      return "capability_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::mapping_uuid_required:
      return "mapping_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::reference_profile_uuid_required:
      return "reference_profile_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::
        canonical_descriptor_uuid_required:
      return "canonical_descriptor_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::capability_epoch_required:
      return "capability_epoch_required";
    case ExecutionTypeCapabilityDescriptorStatus::stable_name_required:
      return "stable_name_required";
    case ExecutionTypeCapabilityDescriptorStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionTypeCapabilityDescriptorStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ExecutionTypeCapabilityDescriptorStatus::mapping_descriptor_invalid:
      return "mapping_descriptor_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::mapping_uuid_mismatch:
      return "mapping_uuid_mismatch";
    case ExecutionTypeCapabilityDescriptorStatus::reference_profile_uuid_mismatch:
      return "reference_profile_uuid_mismatch";
    case ExecutionTypeCapabilityDescriptorStatus::
        canonical_descriptor_uuid_mismatch:
      return "canonical_descriptor_uuid_mismatch";
    case ExecutionTypeCapabilityDescriptorStatus::literal_policy_invalid:
      return "literal_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::bind_policy_invalid:
      return "bind_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::inference_policy_invalid:
      return "inference_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::overload_policy_invalid:
      return "overload_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::cast_policy_invalid:
      return "cast_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::operation_policy_invalid:
      return "operation_policy_invalid";
    case ExecutionTypeCapabilityDescriptorStatus::
        active_capability_requires_supported_mapping:
      return "active_capability_requires_supported_mapping";
    case ExecutionTypeCapabilityDescriptorStatus::
        cast_capability_requires_cast_rule:
      return "cast_capability_requires_cast_rule";
    case ExecutionTypeCapabilityDescriptorStatus::
        operation_capability_requires_operation_descriptor:
      return "operation_capability_requires_operation_descriptor";
    case ExecutionTypeCapabilityDescriptorStatus::
        cpp_udr_mapping_descriptor_uuid_required:
      return "cpp_udr_mapping_descriptor_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::
        cpp_udr_mapping_descriptor_epoch_required:
      return "cpp_udr_mapping_descriptor_epoch_required";
    case ExecutionTypeCapabilityDescriptorStatus::
        llvm_acceleration_requires_cpp_udr_bridge:
      return "llvm_acceleration_requires_cpp_udr_bridge";
    case ExecutionTypeCapabilityDescriptorStatus::
        llvm_acceleration_descriptor_uuid_required:
      return "llvm_acceleration_descriptor_uuid_required";
    case ExecutionTypeCapabilityDescriptorStatus::
        llvm_acceleration_descriptor_epoch_required:
      return "llvm_acceleration_descriptor_epoch_required";
    case ExecutionTypeCapabilityDescriptorStatus::external_locator_policy_required:
      return "external_locator_policy_required";
    case ExecutionTypeCapabilityDescriptorStatus::opaque_lifecycle_required:
      return "opaque_lifecycle_required";
    case ExecutionTypeCapabilityDescriptorStatus::
        reference_superiority_matrix_required:
      return "reference_superiority_matrix_required";
  }
  return "unknown_status";
}

struct ExecutionTypeCapabilityDescriptorValidationResult {
  ExecutionTypeCapabilityDescriptorStatus status =
      ExecutionTypeCapabilityDescriptorStatus::ok;
  ReferenceTypeMappingDescriptorStatus mapping_status =
      ReferenceTypeMappingDescriptorStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionTypeCapabilityDescriptorStatus::ok;
  }
};

constexpr bool ExecutionTypeCapabilityUsesCppUdr(
    const ExecutionTypeCapabilityDescriptor& descriptor) noexcept {
  return descriptor.cxx_udr_bridge_available ||
         ExecutionTypeCapabilityStateRequiresCppUdr(
             descriptor.literal_policy) ||
         ExecutionTypeCapabilityStateRequiresCppUdr(descriptor.bind_policy) ||
         ExecutionTypeCapabilityStateRequiresCppUdr(
             descriptor.inference_policy) ||
         ExecutionTypeCapabilityStateRequiresCppUdr(
             descriptor.overload_policy) ||
         ExecutionTypeCapabilityStateRequiresCppUdr(descriptor.cast_policy) ||
         ExecutionTypeCapabilityStateRequiresCppUdr(
             descriptor.operation_policy);
}

constexpr bool ExecutionTypeCapabilityUsesLlvm(
    const ExecutionTypeCapabilityDescriptor& descriptor) noexcept {
  return descriptor.llvm_acceleration_available ||
         ExecutionTypeCapabilityStateRequiresLlvm(
             descriptor.literal_policy) ||
         ExecutionTypeCapabilityStateRequiresLlvm(descriptor.bind_policy) ||
         ExecutionTypeCapabilityStateRequiresLlvm(
             descriptor.inference_policy) ||
         ExecutionTypeCapabilityStateRequiresLlvm(
             descriptor.overload_policy) ||
         ExecutionTypeCapabilityStateRequiresLlvm(descriptor.cast_policy) ||
         ExecutionTypeCapabilityStateRequiresLlvm(
             descriptor.operation_policy);
}

constexpr bool ExecutionTypeCapabilityHasActiveCapability(
    const ExecutionTypeCapabilityDescriptor& descriptor) noexcept {
  return ExecutionTypeCapabilityStateIsActive(descriptor.literal_policy) ||
         ExecutionTypeCapabilityStateIsActive(descriptor.bind_policy) ||
         ExecutionTypeCapabilityStateIsActive(descriptor.inference_policy) ||
         ExecutionTypeCapabilityStateIsActive(descriptor.overload_policy) ||
         ExecutionTypeCapabilityStateIsActive(descriptor.cast_policy) ||
         ExecutionTypeCapabilityStateIsActive(descriptor.operation_policy);
}

inline ExecutionTypeCapabilityDescriptorValidationResult
ValidateExecutionTypeCapabilityDescriptor(
    const ReferenceTypeMappingDescriptor& mapping,
    const ExecutionTypeCapabilityDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.capability_uuid)) {
    return {ExecutionTypeCapabilityDescriptorStatus::capability_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.mapping_uuid)) {
    return {ExecutionTypeCapabilityDescriptorStatus::mapping_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.reference_profile_uuid)) {
    return {
        ExecutionTypeCapabilityDescriptorStatus::reference_profile_uuid_required};
  }
  if (descriptor.capability_epoch == 0) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                capability_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ExecutionTypeCapabilityDescriptorStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                descriptor_parser_dependent};
  }

  const auto mapping_result = ValidateReferenceTypeMappingDescriptor(mapping);
  if (!mapping_result.ok()) {
    return {ExecutionTypeCapabilityDescriptorStatus::mapping_descriptor_invalid,
            mapping_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.mapping_uuid,
                                    mapping.mapping_uuid)) {
    return {ExecutionTypeCapabilityDescriptorStatus::mapping_uuid_mismatch};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.reference_profile_uuid,
                                    mapping.reference_profile_uuid)) {
    return {
        ExecutionTypeCapabilityDescriptorStatus::reference_profile_uuid_mismatch};
  }
  if (ReferenceTypeRepresentationIsExecutable(mapping.representation_class)) {
    if (ExecutionDataPacketUuidIsNil(descriptor.canonical_descriptor_uuid)) {
      return {ExecutionTypeCapabilityDescriptorStatus::
                  canonical_descriptor_uuid_required};
    }
    if (!ExecutionDataPacketUuidEquals(descriptor.canonical_descriptor_uuid,
                                      mapping.canonical_descriptor_uuid)) {
      return {ExecutionTypeCapabilityDescriptorStatus::
                  canonical_descriptor_uuid_mismatch};
    }
  }

  if (!ExecutionTypeCapabilityStateIsValid(descriptor.literal_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::literal_policy_invalid};
  }
  if (!ExecutionTypeCapabilityStateIsValid(descriptor.bind_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::bind_policy_invalid};
  }
  if (!ExecutionTypeCapabilityStateIsValid(descriptor.inference_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::inference_policy_invalid};
  }
  if (!ExecutionTypeCapabilityStateIsValid(descriptor.overload_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::overload_policy_invalid};
  }
  if (!ExecutionTypeCapabilityStateIsValid(descriptor.cast_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::cast_policy_invalid};
  }
  if (!ExecutionTypeCapabilityStateIsValid(descriptor.operation_policy)) {
    return {ExecutionTypeCapabilityDescriptorStatus::operation_policy_invalid};
  }

  const bool mapping_executable =
      ReferenceTypeRepresentationIsExecutable(mapping.representation_class);
  if (!mapping_executable &&
      ExecutionTypeCapabilityHasActiveCapability(descriptor)) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                active_capability_requires_supported_mapping};
  }
  if (!mapping_executable) {
    return {};
  }

  if (ExecutionTypeCapabilityStateIsActive(descriptor.cast_policy) &&
      !mapping.cast_rule_present) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                cast_capability_requires_cast_rule};
  }
  if (ExecutionTypeCapabilityStateIsActive(descriptor.operation_policy) &&
      !mapping.operation_descriptor_present) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                operation_capability_requires_operation_descriptor};
  }

  const bool uses_cpp_udr = ExecutionTypeCapabilityUsesCppUdr(descriptor);
  if (uses_cpp_udr &&
      ExecutionDataPacketUuidIsNil(
          descriptor.cxx_udr_mapping_descriptor_uuid)) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                cpp_udr_mapping_descriptor_uuid_required};
  }
  if (uses_cpp_udr && descriptor.cxx_udr_mapping_descriptor_epoch == 0) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                cpp_udr_mapping_descriptor_epoch_required};
  }

  const bool uses_llvm = ExecutionTypeCapabilityUsesLlvm(descriptor);
  if (uses_llvm && !descriptor.cxx_udr_bridge_available) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                llvm_acceleration_requires_cpp_udr_bridge};
  }
  if (uses_llvm &&
      ExecutionDataPacketUuidIsNil(
          descriptor.llvm_acceleration_descriptor_uuid)) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                llvm_acceleration_descriptor_uuid_required};
  }
  if (uses_llvm && descriptor.llvm_acceleration_descriptor_epoch == 0) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                llvm_acceleration_descriptor_epoch_required};
  }

  if ((mapping.representation_class ==
           ReferenceTypeRepresentationClass::external_locator ||
       descriptor.external_locator_allowed) &&
      (!descriptor.external_locator_allowed ||
       !descriptor.external_locator_policy_safe ||
       !mapping.external_locator_safe)) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                external_locator_policy_required};
  }
  if (mapping.representation_class ==
          ReferenceTypeRepresentationClass::opaque_bridge &&
      (!descriptor.opaque_lifecycle_managed ||
       !mapping.opaque_lifecycle_managed)) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                opaque_lifecycle_required};
  }
  if (!descriptor.reference_superiority_matrix_entry_present) {
    return {ExecutionTypeCapabilityDescriptorStatus::
                reference_superiority_matrix_required};
  }

  return {};
}

// SEARCH_KEY: TOR-TYPE-OPERATION-REGISTRY
// SEARCH_KEY: TOR-OPERATION-DESCRIPTOR-FIELDS
// SEARCH_KEY: TOR-OPERATION-REGISTRATION
// SEARCH_KEY: TOR-OVERLOAD-RESOLUTION
// SEARCH_KEY: TOR-CAST-REGISTRY
// SEARCH_KEY: TOR-AGGREGATE-WINDOW-STATE
// SEARCH_KEY: TOR-SBLR-OPERATION-BINDING
enum class TypeOperationRegistryRowStatus : std::uint8_t {
  proposed = 0,
  active = 1,
  disabled_by_policy = 2,
  bridge_only = 3,
  render_only = 4,
  deferred = 5,
  retired = 6,
  unsupported_by_policy = 7,
  quarantined = 8
};

constexpr bool TypeOperationRegistryRowStatusIsValid(
    TypeOperationRegistryRowStatus status) noexcept {
  switch (status) {
    case TypeOperationRegistryRowStatus::proposed:
    case TypeOperationRegistryRowStatus::active:
    case TypeOperationRegistryRowStatus::disabled_by_policy:
    case TypeOperationRegistryRowStatus::bridge_only:
    case TypeOperationRegistryRowStatus::render_only:
    case TypeOperationRegistryRowStatus::deferred:
    case TypeOperationRegistryRowStatus::retired:
    case TypeOperationRegistryRowStatus::unsupported_by_policy:
    case TypeOperationRegistryRowStatus::quarantined:
      return true;
  }
  return false;
}

constexpr bool TypeOperationRegistryRowStatusIsExecutable(
    TypeOperationRegistryRowStatus status) noexcept {
  return status == TypeOperationRegistryRowStatus::active ||
         status == TypeOperationRegistryRowStatus::bridge_only;
}

constexpr bool TypeOperationRegistryRowStatusRequiresDiagnostic(
    TypeOperationRegistryRowStatus status) noexcept {
  switch (status) {
    case TypeOperationRegistryRowStatus::disabled_by_policy:
    case TypeOperationRegistryRowStatus::render_only:
    case TypeOperationRegistryRowStatus::deferred:
    case TypeOperationRegistryRowStatus::retired:
    case TypeOperationRegistryRowStatus::unsupported_by_policy:
    case TypeOperationRegistryRowStatus::quarantined:
      return true;
    case TypeOperationRegistryRowStatus::proposed:
    case TypeOperationRegistryRowStatus::active:
    case TypeOperationRegistryRowStatus::bridge_only:
      return false;
  }
  return false;
}

enum class TypeOperationKind : std::uint8_t {
  operation_family = 0,
  type_operator = 1,
  type_function = 2,
  type_cast = 3,
  type_aggregate = 4,
  type_window = 5,
  domain_operator = 6,
  domain_function = 7,
  domain_cast = 8,
  domain_operation = 9,
  reference_method = 10
};

constexpr bool TypeOperationKindIsValid(TypeOperationKind kind) noexcept {
  switch (kind) {
    case TypeOperationKind::operation_family:
    case TypeOperationKind::type_operator:
    case TypeOperationKind::type_function:
    case TypeOperationKind::type_cast:
    case TypeOperationKind::type_aggregate:
    case TypeOperationKind::type_window:
    case TypeOperationKind::domain_operator:
    case TypeOperationKind::domain_function:
    case TypeOperationKind::domain_cast:
    case TypeOperationKind::domain_operation:
    case TypeOperationKind::reference_method:
      return true;
  }
  return false;
}

constexpr bool TypeOperationKindRequiresDomainCast(
    TypeOperationKind kind) noexcept {
  return kind == TypeOperationKind::domain_cast;
}

constexpr bool TypeOperationKindRequiresDomainOperation(
    TypeOperationKind kind) noexcept {
  return kind == TypeOperationKind::domain_operator ||
         kind == TypeOperationKind::domain_function ||
         kind == TypeOperationKind::domain_operation ||
         kind == TypeOperationKind::reference_method;
}

constexpr bool TypeOperationKindRequiresAggregateState(
    TypeOperationKind kind) noexcept {
  return kind == TypeOperationKind::type_aggregate ||
         kind == TypeOperationKind::type_window;
}

constexpr bool TypeOperationKindRequiresWindowFrame(
    TypeOperationKind kind) noexcept {
  return kind == TypeOperationKind::type_window;
}

enum class TypeOperationImplementationTarget : std::uint8_t {
  none = 0,
  internal_engine = 1,
  native_sblr = 2,
  sblr_routine = 3,
  cpp_udr = 4,
  llvm_native = 5,
  reference_native = 6
};

constexpr bool TypeOperationImplementationTargetIsValid(
    TypeOperationImplementationTarget target) noexcept {
  switch (target) {
    case TypeOperationImplementationTarget::none:
    case TypeOperationImplementationTarget::internal_engine:
    case TypeOperationImplementationTarget::native_sblr:
    case TypeOperationImplementationTarget::sblr_routine:
    case TypeOperationImplementationTarget::cpp_udr:
    case TypeOperationImplementationTarget::llvm_native:
    case TypeOperationImplementationTarget::reference_native:
      return true;
  }
  return false;
}

constexpr bool TypeOperationImplementationTargetRequiresCppUdr(
    TypeOperationImplementationTarget target) noexcept {
  return target == TypeOperationImplementationTarget::cpp_udr ||
         target == TypeOperationImplementationTarget::llvm_native;
}

constexpr bool TypeOperationImplementationTargetRequiresLlvm(
    TypeOperationImplementationTarget target) noexcept {
  return target == TypeOperationImplementationTarget::llvm_native;
}

enum class TypeOperationCastClass : std::uint8_t {
  none = 0,
  implicit = 1,
  assignment = 2,
  explicit_cast = 3,
  parser_compat = 4,
  udr_bridge = 5,
  storage_read = 6,
  storage_write = 7,
  wire_render = 8,
  diagnostic_render = 9
};

constexpr bool TypeOperationCastClassIsValid(
    TypeOperationCastClass cast_class) noexcept {
  switch (cast_class) {
    case TypeOperationCastClass::none:
    case TypeOperationCastClass::implicit:
    case TypeOperationCastClass::assignment:
    case TypeOperationCastClass::explicit_cast:
    case TypeOperationCastClass::parser_compat:
    case TypeOperationCastClass::udr_bridge:
    case TypeOperationCastClass::storage_read:
    case TypeOperationCastClass::storage_write:
    case TypeOperationCastClass::wire_render:
    case TypeOperationCastClass::diagnostic_render:
      return true;
  }
  return false;
}

enum class TypeOperationDomainStackPolicy : std::uint8_t {
  preserve = 0,
  strip = 1,
  transform = 2,
  reject = 3
};

constexpr bool TypeOperationDomainStackPolicyIsValid(
    TypeOperationDomainStackPolicy policy) noexcept {
  switch (policy) {
    case TypeOperationDomainStackPolicy::preserve:
    case TypeOperationDomainStackPolicy::strip:
    case TypeOperationDomainStackPolicy::transform:
    case TypeOperationDomainStackPolicy::reject:
      return true;
  }
  return false;
}

inline constexpr std::size_t kTypeOperationRegistryMaxOperands = 16;

struct TypeOperationOverloadDescriptor {
  Uuid overload_set_uuid{};
  std::uint64_t overload_epoch = 0;
  std::string overload_key;
  std::vector<Uuid> argument_descriptor_uuids;
  std::vector<Uuid> argument_domain_uuids;
  std::vector<ExecutionTypeDescriptor> argument_descriptors;
  Uuid result_descriptor_uuid{};
  Uuid result_domain_uuid{};
  ExecutionTypeDescriptor result_descriptor;
  bool literal_bind_typing_declared = true;
  bool ambiguity_fails_closed = true;
  bool security_invisible_candidates_hidden = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct AggregateStateDescriptor {
  Uuid state_descriptor_uuid{};
  Uuid transition_function_uuid{};
  Uuid final_function_uuid{};
  Uuid combine_function_uuid{};
  Uuid inverse_function_uuid{};
  Uuid serialization_function_uuid{};
  Uuid deserialization_function_uuid{};
  Uuid memory_class_uuid{};
  Uuid spill_policy_uuid{};
  Uuid cleanup_policy_uuid{};
  std::uint64_t state_version = 0;
  std::uint64_t serialization_version = 0;
  ExecutionTypeDescriptor state_descriptor;
  bool serializable = true;
  bool cleanup_safe = true;
  bool parallel_combine_allowed = false;
  bool combine_associativity_proven = false;
  bool inverse_allowed = false;
  bool inverse_correctness_proven = false;
};

struct WindowFrameDescriptor {
  Uuid frame_policy_uuid{};
  std::uint64_t frame_policy_epoch = 0;
  bool ordering_dependency_declared = true;
  bool peer_behavior_declared = true;
  bool null_missing_policy_declared = true;
  bool frame_removal_allowed = false;
  Uuid inverse_function_uuid{};
};

struct TypeOperationSblrBindingDescriptor {
  Uuid operation_uuid{};
  Uuid operation_family_uuid{};
  std::uint64_t implementation_version = 0;
  std::vector<Uuid> argument_descriptor_uuids;
  std::vector<Uuid> argument_domain_uuids;
  Uuid result_descriptor_uuid{};
  Uuid result_domain_uuid{};
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::string definition_hash;
  Uuid reference_profile_uuid{};
  Uuid fallback_reference_uuid{};
  std::string diagnostic_search_key;
  bool source_sql_diagnostic_only = true;
  bool runtime_status_recheck = true;
  bool runtime_security_recheck = true;
  bool runtime_resource_epoch_recheck = true;
  bool runtime_implementation_version_recheck = true;
  bool runtime_definition_hash_recheck = true;
  bool executable_outside_owner_transaction = false;
};

struct TypeOperationCacheKeyDescriptor {
  Uuid operation_uuid{};
  Uuid operation_family_uuid{};
  std::vector<Uuid> argument_descriptor_uuids;
  std::vector<Uuid> argument_domain_uuids;
  Uuid result_descriptor_uuid{};
  Uuid result_domain_uuid{};
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t implementation_version = 0;
  std::string definition_hash;
  Uuid cpp_udr_package_uuid{};
  std::uint64_t cpp_udr_package_version = 0;
  Uuid llvm_artifact_uuid{};
  std::uint64_t llvm_artifact_version = 0;
  Uuid reference_profile_uuid{};
};

struct TypeOperationDiagnosticVector {
  std::string diagnostic_code;
  Uuid operation_uuid{};
  Uuid operation_family_uuid{};
  TypeOperationRegistryRowStatus row_status =
      TypeOperationRegistryRowStatus::active;
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t implementation_version = 0;
  Uuid reference_profile_uuid{};
  bool redaction_state_declared = true;
};

struct TypeOperationRegistryEntry {
  Uuid operation_uuid{};
  Uuid operation_family_uuid{};
  Uuid schema_uuid{};
  Uuid owning_package_uuid{};
  Uuid name_ref_uuid{};
  Uuid owner_transaction_uuid{};
  std::uint64_t operation_epoch = 0;
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t implementation_version = 0;
  std::string stable_name;
  std::string reference_family;
  std::string reference_version_profile;
  std::string definition_hash;
  std::string diagnostic_search_key;
  std::string conformance_key;
  TypeOperationKind operation_kind = TypeOperationKind::type_function;
  TypeOperationRegistryRowStatus row_status =
      TypeOperationRegistryRowStatus::active;
  TypeOperationImplementationTarget implementation_target =
      TypeOperationImplementationTarget::internal_engine;
  bool implementation_target_present = true;
  bool non_cpp_udr_runtime_requested = false;
  TypeOperationDomainStackPolicy domain_stack_policy =
      TypeOperationDomainStackPolicy::preserve;
  DomainNullPolicy null_policy = DomainNullPolicy::propagate_null;
  DomainMissingPolicy missing_policy = DomainMissingPolicy::propagate_missing;
  bool default_value_policy_declared = true;
  bool unknown_value_policy_declared = true;
  bool error_value_policy_declared = true;
  TypeOperationCastClass cast_class = TypeOperationCastClass::none;
  Uuid cost_model_uuid{};
  Uuid selectivity_model_uuid{};
  Uuid index_eligibility_uuid{};
  TypeOperationOverloadDescriptor overload;
  bool domain_cast_rule_present = false;
  Uuid domain_cast_rule_uuid{};
  DomainCastRuleDescriptor domain_cast_rule;
  bool domain_operation_present = false;
  Uuid domain_operation_uuid{};
  DomainOperationDescriptor domain_operation;
  bool aggregate_state_present = false;
  AggregateStateDescriptor aggregate_state;
  bool window_frame_present = false;
  WindowFrameDescriptor window_frame;
  DomainCppUdrOperationHook cpp_udr_hook;
  bool cpp_udr_bridge_loaded = true;
  bool cpp_udr_bridge_admitted = true;
  Uuid cpp_udr_package_uuid{};
  std::uint64_t cpp_udr_package_version = 0;
  Uuid llvm_artifact_uuid{};
  std::uint64_t llvm_artifact_epoch = 0;
  std::uint64_t llvm_artifact_version = 0;
  Uuid llvm_fallback_reference_uuid{};
  bool llvm_fallback_matches = true;
  bool llvm_invalidates_with_artifact = true;
  bool transactional_registration = true;
  bool rollback_safe = true;
  bool dependency_invalidation_registered = true;
  bool cache_invalidation_registered = true;
  bool idempotent_definition_hash_checked = true;
  bool status_execution_refusal_diagnostic = true;
  bool sblr_binding_present = true;
  TypeOperationSblrBindingDescriptor sblr_binding;
  bool cache_key_present = true;
  TypeOperationCacheKeyDescriptor cache_key;
  bool reference_method_binding_present = false;
  Uuid reference_profile_uuid{};
  std::string reference_method_name;
  std::string inverse_rendering_policy;
  std::vector<TypeOperationDiagnosticVector> diagnostics;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct TypeOperationRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string stable_name;
  Uuid catalog_snapshot_uuid{};
  Uuid visible_transaction_uuid{};
  std::vector<TypeOperationRegistryEntry> entries;
  std::vector<std::string> local_metric_names;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool committed_catalog_visible = true;
  bool local_metrics_root_declared = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
};

enum class TypeOperationRegistryStatus : std::uint8_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  stable_name_required = 3,
  catalog_snapshot_uuid_required = 4,
  visible_transaction_uuid_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  committed_catalog_not_visible = 8,
  entries_required = 9,
  operation_uuid_required = 10,
  operation_family_uuid_required = 11,
  duplicate_operation_uuid = 12,
  schema_uuid_required = 13,
  owning_package_uuid_required = 14,
  name_ref_uuid_required = 15,
  operation_epoch_required = 16,
  schema_epoch_required = 17,
  security_epoch_required = 18,
  resource_epoch_required = 19,
  implementation_version_required = 20,
  definition_hash_required = 21,
  diagnostic_search_key_required = 22,
  conformance_key_required = 23,
  entry_stable_name_required = 24,
  entry_descriptor_not_authoritative = 25,
  entry_descriptor_parser_dependent = 26,
  operation_kind_invalid = 27,
  row_status_invalid = 28,
  implementation_target_invalid = 29,
  implementation_target_missing = 30,
  non_cpp_udr_forbidden = 31,
  status_refusal_diagnostic_required = 32,
  proposed_owner_transaction_required = 33,
  proposed_row_executable_outside_owner = 34,
  transactional_registration_required = 35,
  rollback_safe_registration_required = 36,
  dependency_invalidation_required = 37,
  cache_invalidation_required = 38,
  idempotent_definition_hash_required = 39,
  domain_stack_policy_invalid = 40,
  null_policy_invalid = 41,
  missing_policy_invalid = 42,
  value_policy_required = 43,
  overload_uuid_required = 44,
  overload_epoch_required = 45,
  overload_key_required = 46,
  overload_descriptor_not_authoritative = 47,
  overload_descriptor_parser_dependent = 48,
  overload_ambiguity_fail_closed_required = 49,
  overload_security_hiding_required = 50,
  duplicate_overload_signature = 51,
  operand_descriptor_count_mismatch = 52,
  operand_descriptor_count_exceeds_limit = 53,
  operand_descriptor_uuid_required = 54,
  operand_descriptor_invalid = 55,
  operand_descriptor_uuid_mismatch = 56,
  result_descriptor_uuid_required = 57,
  result_descriptor_invalid = 58,
  result_descriptor_uuid_mismatch = 59,
  cast_class_invalid = 60,
  cast_class_required = 61,
  domain_cast_descriptor_required = 62,
  domain_cast_descriptor_invalid = 63,
  domain_cast_uuid_mismatch = 64,
  domain_operation_descriptor_required = 65,
  domain_operation_descriptor_invalid = 66,
  domain_operation_uuid_mismatch = 67,
  aggregate_state_required = 68,
  aggregate_state_uuid_required = 69,
  aggregate_state_version_required = 70,
  aggregate_state_descriptor_invalid = 71,
  aggregate_state_uuid_mismatch = 72,
  aggregate_transition_required = 73,
  aggregate_final_required = 74,
  aggregate_memory_policy_required = 75,
  aggregate_cleanup_policy_required = 76,
  aggregate_serialization_version_required = 77,
  aggregate_parallel_proof_required = 78,
  window_frame_required = 79,
  window_frame_policy_required = 80,
  window_frame_policy_epoch_required = 81,
  window_ordering_policy_required = 82,
  window_inverse_required = 83,
  cpp_udr_hook_required = 84,
  cpp_udr_library_uuid_required = 85,
  cpp_udr_mapping_descriptor_uuid_required = 86,
  cpp_udr_mapping_descriptor_epoch_required = 87,
  cpp_udr_entrypoint_required = 88,
  cpp_udr_abi_required = 89,
  cpp_udr_descriptor_preservation_required = 90,
  cpp_udr_parser_independent_required = 91,
  cpp_udr_bridge_admission_required = 92,
  llvm_artifact_uuid_required = 93,
  llvm_artifact_epoch_required = 94,
  llvm_artifact_version_required = 95,
  llvm_fallback_required = 96,
  llvm_fallback_mismatch = 97,
  llvm_invalidation_required = 98,
  sblr_binding_required = 99,
  sblr_binding_operation_uuid_mismatch = 100,
  sblr_binding_family_uuid_mismatch = 101,
  sblr_binding_implementation_version_mismatch = 102,
  sblr_binding_schema_epoch_mismatch = 103,
  sblr_binding_security_epoch_mismatch = 104,
  sblr_binding_resource_epoch_mismatch = 105,
  sblr_binding_definition_hash_mismatch = 106,
  sblr_binding_argument_count_mismatch = 107,
  sblr_binding_argument_uuid_mismatch = 108,
  sblr_binding_result_uuid_mismatch = 109,
  sblr_binding_diagnostic_key_required = 110,
  sblr_binding_recheck_required = 111,
  sblr_source_sql_not_diagnostic_only = 112,
  cache_key_required = 113,
  cache_key_operation_uuid_mismatch = 114,
  cache_key_family_uuid_mismatch = 115,
  cache_key_epoch_mismatch = 116,
  cache_key_implementation_version_mismatch = 117,
  cache_key_definition_hash_mismatch = 118,
  cache_key_argument_uuid_mismatch = 119,
  cache_key_result_uuid_mismatch = 120,
  cache_key_cpp_udr_missing = 121,
  cache_key_llvm_missing = 122,
  cache_key_reference_profile_missing = 123,
  reference_method_binding_invalid = 124,
  diagnostic_vector_required = 125,
  diagnostic_code_required = 126,
  diagnostic_redaction_state_required = 127,
  local_metrics_root_required = 128,
  local_metric_missing = 129,
  cluster_metrics_guard_required = 130
};

constexpr std::string_view TypeOperationRegistryStatusName(
    TypeOperationRegistryStatus status) noexcept {
  switch (status) {
    case TypeOperationRegistryStatus::ok:
      return "ok";
    case TypeOperationRegistryStatus::registry_uuid_required:
      return "registry_uuid_required";
    case TypeOperationRegistryStatus::registry_epoch_required:
      return "registry_epoch_required";
    case TypeOperationRegistryStatus::stable_name_required:
      return "stable_name_required";
    case TypeOperationRegistryStatus::catalog_snapshot_uuid_required:
      return "catalog_snapshot_uuid_required";
    case TypeOperationRegistryStatus::visible_transaction_uuid_required:
      return "visible_transaction_uuid_required";
    case TypeOperationRegistryStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case TypeOperationRegistryStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case TypeOperationRegistryStatus::committed_catalog_not_visible:
      return "committed_catalog_not_visible";
    case TypeOperationRegistryStatus::entries_required:
      return "entries_required";
    case TypeOperationRegistryStatus::operation_uuid_required:
      return "operation_uuid_required";
    case TypeOperationRegistryStatus::operation_family_uuid_required:
      return "operation_family_uuid_required";
    case TypeOperationRegistryStatus::duplicate_operation_uuid:
      return "duplicate_operation_uuid";
    case TypeOperationRegistryStatus::schema_uuid_required:
      return "schema_uuid_required";
    case TypeOperationRegistryStatus::owning_package_uuid_required:
      return "owning_package_uuid_required";
    case TypeOperationRegistryStatus::name_ref_uuid_required:
      return "name_ref_uuid_required";
    case TypeOperationRegistryStatus::operation_epoch_required:
      return "operation_epoch_required";
    case TypeOperationRegistryStatus::schema_epoch_required:
      return "schema_epoch_required";
    case TypeOperationRegistryStatus::security_epoch_required:
      return "security_epoch_required";
    case TypeOperationRegistryStatus::resource_epoch_required:
      return "resource_epoch_required";
    case TypeOperationRegistryStatus::implementation_version_required:
      return "implementation_version_required";
    case TypeOperationRegistryStatus::definition_hash_required:
      return "definition_hash_required";
    case TypeOperationRegistryStatus::diagnostic_search_key_required:
      return "diagnostic_search_key_required";
    case TypeOperationRegistryStatus::conformance_key_required:
      return "conformance_key_required";
    case TypeOperationRegistryStatus::entry_stable_name_required:
      return "entry_stable_name_required";
    case TypeOperationRegistryStatus::entry_descriptor_not_authoritative:
      return "entry_descriptor_not_authoritative";
    case TypeOperationRegistryStatus::entry_descriptor_parser_dependent:
      return "entry_descriptor_parser_dependent";
    case TypeOperationRegistryStatus::operation_kind_invalid:
      return "operation_kind_invalid";
    case TypeOperationRegistryStatus::row_status_invalid:
      return "row_status_invalid";
    case TypeOperationRegistryStatus::implementation_target_invalid:
      return "implementation_target_invalid";
    case TypeOperationRegistryStatus::implementation_target_missing:
      return "implementation_target_missing";
    case TypeOperationRegistryStatus::non_cpp_udr_forbidden:
      return "non_cpp_udr_forbidden";
    case TypeOperationRegistryStatus::status_refusal_diagnostic_required:
      return "status_refusal_diagnostic_required";
    case TypeOperationRegistryStatus::proposed_owner_transaction_required:
      return "proposed_owner_transaction_required";
    case TypeOperationRegistryStatus::proposed_row_executable_outside_owner:
      return "proposed_row_executable_outside_owner";
    case TypeOperationRegistryStatus::transactional_registration_required:
      return "transactional_registration_required";
    case TypeOperationRegistryStatus::rollback_safe_registration_required:
      return "rollback_safe_registration_required";
    case TypeOperationRegistryStatus::dependency_invalidation_required:
      return "dependency_invalidation_required";
    case TypeOperationRegistryStatus::cache_invalidation_required:
      return "cache_invalidation_required";
    case TypeOperationRegistryStatus::idempotent_definition_hash_required:
      return "idempotent_definition_hash_required";
    case TypeOperationRegistryStatus::domain_stack_policy_invalid:
      return "domain_stack_policy_invalid";
    case TypeOperationRegistryStatus::null_policy_invalid:
      return "null_policy_invalid";
    case TypeOperationRegistryStatus::missing_policy_invalid:
      return "missing_policy_invalid";
    case TypeOperationRegistryStatus::value_policy_required:
      return "value_policy_required";
    case TypeOperationRegistryStatus::overload_uuid_required:
      return "overload_uuid_required";
    case TypeOperationRegistryStatus::overload_epoch_required:
      return "overload_epoch_required";
    case TypeOperationRegistryStatus::overload_key_required:
      return "overload_key_required";
    case TypeOperationRegistryStatus::overload_descriptor_not_authoritative:
      return "overload_descriptor_not_authoritative";
    case TypeOperationRegistryStatus::overload_descriptor_parser_dependent:
      return "overload_descriptor_parser_dependent";
    case TypeOperationRegistryStatus::overload_ambiguity_fail_closed_required:
      return "overload_ambiguity_fail_closed_required";
    case TypeOperationRegistryStatus::overload_security_hiding_required:
      return "overload_security_hiding_required";
    case TypeOperationRegistryStatus::duplicate_overload_signature:
      return "duplicate_overload_signature";
    case TypeOperationRegistryStatus::operand_descriptor_count_mismatch:
      return "operand_descriptor_count_mismatch";
    case TypeOperationRegistryStatus::operand_descriptor_count_exceeds_limit:
      return "operand_descriptor_count_exceeds_limit";
    case TypeOperationRegistryStatus::operand_descriptor_uuid_required:
      return "operand_descriptor_uuid_required";
    case TypeOperationRegistryStatus::operand_descriptor_invalid:
      return "operand_descriptor_invalid";
    case TypeOperationRegistryStatus::operand_descriptor_uuid_mismatch:
      return "operand_descriptor_uuid_mismatch";
    case TypeOperationRegistryStatus::result_descriptor_uuid_required:
      return "result_descriptor_uuid_required";
    case TypeOperationRegistryStatus::result_descriptor_invalid:
      return "result_descriptor_invalid";
    case TypeOperationRegistryStatus::result_descriptor_uuid_mismatch:
      return "result_descriptor_uuid_mismatch";
    case TypeOperationRegistryStatus::cast_class_invalid:
      return "cast_class_invalid";
    case TypeOperationRegistryStatus::cast_class_required:
      return "cast_class_required";
    case TypeOperationRegistryStatus::domain_cast_descriptor_required:
      return "domain_cast_descriptor_required";
    case TypeOperationRegistryStatus::domain_cast_descriptor_invalid:
      return "domain_cast_descriptor_invalid";
    case TypeOperationRegistryStatus::domain_cast_uuid_mismatch:
      return "domain_cast_uuid_mismatch";
    case TypeOperationRegistryStatus::domain_operation_descriptor_required:
      return "domain_operation_descriptor_required";
    case TypeOperationRegistryStatus::domain_operation_descriptor_invalid:
      return "domain_operation_descriptor_invalid";
    case TypeOperationRegistryStatus::domain_operation_uuid_mismatch:
      return "domain_operation_uuid_mismatch";
    case TypeOperationRegistryStatus::aggregate_state_required:
      return "aggregate_state_required";
    case TypeOperationRegistryStatus::aggregate_state_uuid_required:
      return "aggregate_state_uuid_required";
    case TypeOperationRegistryStatus::aggregate_state_version_required:
      return "aggregate_state_version_required";
    case TypeOperationRegistryStatus::aggregate_state_descriptor_invalid:
      return "aggregate_state_descriptor_invalid";
    case TypeOperationRegistryStatus::aggregate_state_uuid_mismatch:
      return "aggregate_state_uuid_mismatch";
    case TypeOperationRegistryStatus::aggregate_transition_required:
      return "aggregate_transition_required";
    case TypeOperationRegistryStatus::aggregate_final_required:
      return "aggregate_final_required";
    case TypeOperationRegistryStatus::aggregate_memory_policy_required:
      return "aggregate_memory_policy_required";
    case TypeOperationRegistryStatus::aggregate_cleanup_policy_required:
      return "aggregate_cleanup_policy_required";
    case TypeOperationRegistryStatus::
        aggregate_serialization_version_required:
      return "aggregate_serialization_version_required";
    case TypeOperationRegistryStatus::aggregate_parallel_proof_required:
      return "aggregate_parallel_proof_required";
    case TypeOperationRegistryStatus::window_frame_required:
      return "window_frame_required";
    case TypeOperationRegistryStatus::window_frame_policy_required:
      return "window_frame_policy_required";
    case TypeOperationRegistryStatus::window_frame_policy_epoch_required:
      return "window_frame_policy_epoch_required";
    case TypeOperationRegistryStatus::window_ordering_policy_required:
      return "window_ordering_policy_required";
    case TypeOperationRegistryStatus::window_inverse_required:
      return "window_inverse_required";
    case TypeOperationRegistryStatus::cpp_udr_hook_required:
      return "cpp_udr_hook_required";
    case TypeOperationRegistryStatus::cpp_udr_library_uuid_required:
      return "cpp_udr_library_uuid_required";
    case TypeOperationRegistryStatus::
        cpp_udr_mapping_descriptor_uuid_required:
      return "cpp_udr_mapping_descriptor_uuid_required";
    case TypeOperationRegistryStatus::
        cpp_udr_mapping_descriptor_epoch_required:
      return "cpp_udr_mapping_descriptor_epoch_required";
    case TypeOperationRegistryStatus::cpp_udr_entrypoint_required:
      return "cpp_udr_entrypoint_required";
    case TypeOperationRegistryStatus::cpp_udr_abi_required:
      return "cpp_udr_abi_required";
    case TypeOperationRegistryStatus::
        cpp_udr_descriptor_preservation_required:
      return "cpp_udr_descriptor_preservation_required";
    case TypeOperationRegistryStatus::cpp_udr_parser_independent_required:
      return "cpp_udr_parser_independent_required";
    case TypeOperationRegistryStatus::cpp_udr_bridge_admission_required:
      return "cpp_udr_bridge_admission_required";
    case TypeOperationRegistryStatus::llvm_artifact_uuid_required:
      return "llvm_artifact_uuid_required";
    case TypeOperationRegistryStatus::llvm_artifact_epoch_required:
      return "llvm_artifact_epoch_required";
    case TypeOperationRegistryStatus::llvm_artifact_version_required:
      return "llvm_artifact_version_required";
    case TypeOperationRegistryStatus::llvm_fallback_required:
      return "llvm_fallback_required";
    case TypeOperationRegistryStatus::llvm_fallback_mismatch:
      return "llvm_fallback_mismatch";
    case TypeOperationRegistryStatus::llvm_invalidation_required:
      return "llvm_invalidation_required";
    case TypeOperationRegistryStatus::sblr_binding_required:
      return "sblr_binding_required";
    case TypeOperationRegistryStatus::sblr_binding_operation_uuid_mismatch:
      return "sblr_binding_operation_uuid_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_family_uuid_mismatch:
      return "sblr_binding_family_uuid_mismatch";
    case TypeOperationRegistryStatus::
        sblr_binding_implementation_version_mismatch:
      return "sblr_binding_implementation_version_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_schema_epoch_mismatch:
      return "sblr_binding_schema_epoch_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_security_epoch_mismatch:
      return "sblr_binding_security_epoch_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_resource_epoch_mismatch:
      return "sblr_binding_resource_epoch_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_definition_hash_mismatch:
      return "sblr_binding_definition_hash_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_argument_count_mismatch:
      return "sblr_binding_argument_count_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_argument_uuid_mismatch:
      return "sblr_binding_argument_uuid_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_result_uuid_mismatch:
      return "sblr_binding_result_uuid_mismatch";
    case TypeOperationRegistryStatus::sblr_binding_diagnostic_key_required:
      return "sblr_binding_diagnostic_key_required";
    case TypeOperationRegistryStatus::sblr_binding_recheck_required:
      return "sblr_binding_recheck_required";
    case TypeOperationRegistryStatus::sblr_source_sql_not_diagnostic_only:
      return "sblr_source_sql_not_diagnostic_only";
    case TypeOperationRegistryStatus::cache_key_required:
      return "cache_key_required";
    case TypeOperationRegistryStatus::cache_key_operation_uuid_mismatch:
      return "cache_key_operation_uuid_mismatch";
    case TypeOperationRegistryStatus::cache_key_family_uuid_mismatch:
      return "cache_key_family_uuid_mismatch";
    case TypeOperationRegistryStatus::cache_key_epoch_mismatch:
      return "cache_key_epoch_mismatch";
    case TypeOperationRegistryStatus::
        cache_key_implementation_version_mismatch:
      return "cache_key_implementation_version_mismatch";
    case TypeOperationRegistryStatus::cache_key_definition_hash_mismatch:
      return "cache_key_definition_hash_mismatch";
    case TypeOperationRegistryStatus::cache_key_argument_uuid_mismatch:
      return "cache_key_argument_uuid_mismatch";
    case TypeOperationRegistryStatus::cache_key_result_uuid_mismatch:
      return "cache_key_result_uuid_mismatch";
    case TypeOperationRegistryStatus::cache_key_cpp_udr_missing:
      return "cache_key_cpp_udr_missing";
    case TypeOperationRegistryStatus::cache_key_llvm_missing:
      return "cache_key_llvm_missing";
    case TypeOperationRegistryStatus::cache_key_reference_profile_missing:
      return "cache_key_reference_profile_missing";
    case TypeOperationRegistryStatus::reference_method_binding_invalid:
      return "reference_method_binding_invalid";
    case TypeOperationRegistryStatus::diagnostic_vector_required:
      return "diagnostic_vector_required";
    case TypeOperationRegistryStatus::diagnostic_code_required:
      return "diagnostic_code_required";
    case TypeOperationRegistryStatus::diagnostic_redaction_state_required:
      return "diagnostic_redaction_state_required";
    case TypeOperationRegistryStatus::local_metrics_root_required:
      return "local_metrics_root_required";
    case TypeOperationRegistryStatus::local_metric_missing:
      return "local_metric_missing";
    case TypeOperationRegistryStatus::cluster_metrics_guard_required:
      return "cluster_metrics_guard_required";
  }
  return "unknown_status";
}

struct TypeOperationRegistryValidationResult {
  TypeOperationRegistryStatus status = TypeOperationRegistryStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  DomainCastRuleStatus cast_rule_status = DomainCastRuleStatus::ok;
  DomainOperationDescriptorStatus domain_operation_status =
      DomainOperationDescriptorStatus::ok;
  std::size_t entry_index = 0;
  std::size_t operand_index = 0;

  bool ok() const noexcept {
    return status == TypeOperationRegistryStatus::ok;
  }
};

inline bool TypeOperationRegistryUuidVectorEquals(
    const std::vector<Uuid>& left,
    const std::vector<Uuid>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!ExecutionDataPacketUuidEquals(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

inline bool TypeOperationRegistryMetricPresent(
    const TypeOperationRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& candidate : registry.local_metric_names) {
    if (candidate == metric_name) {
      return true;
    }
  }
  return false;
}

inline TypeOperationRegistryValidationResult
ValidateTypeOperationCppUdrHook(const DomainCppUdrOperationHook& hook,
                                std::size_t entry_index) {
  if (!hook.present) {
    return {TypeOperationRegistryStatus::cpp_udr_hook_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(hook.library_uuid)) {
    return {TypeOperationRegistryStatus::cpp_udr_library_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(hook.mapping_descriptor_uuid)) {
    return {TypeOperationRegistryStatus::
                cpp_udr_mapping_descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (hook.mapping_descriptor_epoch == 0) {
    return {TypeOperationRegistryStatus::
                cpp_udr_mapping_descriptor_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (hook.entrypoint_symbol.empty()) {
    return {TypeOperationRegistryStatus::cpp_udr_entrypoint_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (hook.abi_major == 0) {
    return {TypeOperationRegistryStatus::cpp_udr_abi_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!hook.preserves_descriptors) {
    return {TypeOperationRegistryStatus::
                cpp_udr_descriptor_preservation_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!hook.parser_independent) {
    return {TypeOperationRegistryStatus::
                cpp_udr_parser_independent_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult
ValidateAggregateStateDescriptorForRegistry(
    const AggregateStateDescriptor& descriptor,
    std::size_t entry_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.state_descriptor_uuid)) {
    return {TypeOperationRegistryStatus::aggregate_state_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (descriptor.state_version == 0) {
    return {TypeOperationRegistryStatus::aggregate_state_version_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  const auto state_result =
      ValidateExecutionDataPacketDescriptor(descriptor.state_descriptor, 0);
  if (!state_result.ok()) {
    return {TypeOperationRegistryStatus::aggregate_state_descriptor_invalid,
            state_result.status, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.state_descriptor.descriptor_uuid,
          descriptor.state_descriptor_uuid)) {
    return {TypeOperationRegistryStatus::aggregate_state_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.transition_function_uuid)) {
    return {TypeOperationRegistryStatus::aggregate_transition_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.final_function_uuid)) {
    return {TypeOperationRegistryStatus::aggregate_final_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.memory_class_uuid)) {
    return {TypeOperationRegistryStatus::aggregate_memory_policy_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.cleanup_policy_uuid) ||
      !descriptor.cleanup_safe) {
    return {TypeOperationRegistryStatus::aggregate_cleanup_policy_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (descriptor.serializable && descriptor.serialization_version == 0) {
    return {TypeOperationRegistryStatus::
                aggregate_serialization_version_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (descriptor.parallel_combine_allowed &&
      (ExecutionDataPacketUuidIsNil(descriptor.combine_function_uuid) ||
       !descriptor.combine_associativity_proven)) {
    return {TypeOperationRegistryStatus::aggregate_parallel_proof_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult ValidateWindowFrameDescriptorForRegistry(
    const WindowFrameDescriptor& descriptor,
    std::size_t entry_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.frame_policy_uuid)) {
    return {TypeOperationRegistryStatus::window_frame_policy_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (descriptor.frame_policy_epoch == 0) {
    return {TypeOperationRegistryStatus::window_frame_policy_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!descriptor.ordering_dependency_declared ||
      !descriptor.peer_behavior_declared ||
      !descriptor.null_missing_policy_declared) {
    return {TypeOperationRegistryStatus::window_ordering_policy_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (descriptor.frame_removal_allowed &&
      ExecutionDataPacketUuidIsNil(descriptor.inverse_function_uuid)) {
    return {TypeOperationRegistryStatus::window_inverse_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult
ValidateTypeOperationOverloadDescriptor(
    const TypeOperationOverloadDescriptor& overload,
    std::size_t entry_index) {
  if (ExecutionDataPacketUuidIsNil(overload.overload_set_uuid)) {
    return {TypeOperationRegistryStatus::overload_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (overload.overload_epoch == 0) {
    return {TypeOperationRegistryStatus::overload_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (overload.overload_key.empty()) {
    return {TypeOperationRegistryStatus::overload_key_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!overload.descriptor_authoritative) {
    return {TypeOperationRegistryStatus::overload_descriptor_not_authoritative,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!overload.parser_independent) {
    return {TypeOperationRegistryStatus::overload_descriptor_parser_dependent,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!overload.ambiguity_fails_closed) {
    return {TypeOperationRegistryStatus::
                overload_ambiguity_fail_closed_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!overload.security_invisible_candidates_hidden) {
    return {TypeOperationRegistryStatus::overload_security_hiding_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (overload.argument_descriptor_uuids.size() !=
          overload.argument_descriptors.size() ||
      (!overload.argument_domain_uuids.empty() &&
       overload.argument_domain_uuids.size() !=
           overload.argument_descriptors.size())) {
    return {TypeOperationRegistryStatus::operand_descriptor_count_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (overload.argument_descriptors.size() >
      kTypeOperationRegistryMaxOperands) {
    return {TypeOperationRegistryStatus::
                operand_descriptor_count_exceeds_limit,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  for (std::size_t operand_index = 0;
       operand_index < overload.argument_descriptors.size(); ++operand_index) {
    const auto& descriptor = overload.argument_descriptors[operand_index];
    const auto& descriptor_uuid =
        overload.argument_descriptor_uuids[operand_index];
    if (ExecutionDataPacketUuidIsNil(descriptor_uuid)) {
      return {TypeOperationRegistryStatus::operand_descriptor_uuid_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, operand_index};
    }
    const auto descriptor_result =
        ValidateExecutionDataPacketDescriptor(descriptor, operand_index);
    if (!descriptor_result.ok()) {
      return {TypeOperationRegistryStatus::operand_descriptor_invalid,
              descriptor_result.status, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, operand_index};
    }
    if (!ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                      descriptor_uuid)) {
      return {TypeOperationRegistryStatus::operand_descriptor_uuid_mismatch,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, operand_index};
    }
  }
  if (ExecutionDataPacketUuidIsNil(overload.result_descriptor_uuid)) {
    return {TypeOperationRegistryStatus::result_descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  const auto result_descriptor_result =
      ValidateExecutionDataPacketDescriptor(overload.result_descriptor, 0);
  if (!result_descriptor_result.ok()) {
    return {TypeOperationRegistryStatus::result_descriptor_invalid,
            result_descriptor_result.status, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(
          overload.result_descriptor.descriptor_uuid,
          overload.result_descriptor_uuid)) {
    return {TypeOperationRegistryStatus::result_descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult ValidateTypeOperationSblrBinding(
    const TypeOperationRegistryEntry& entry,
    std::size_t entry_index) {
  if (!entry.sblr_binding_present) {
    return {TypeOperationRegistryStatus::sblr_binding_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  const auto& binding = entry.sblr_binding;
  if (!ExecutionDataPacketUuidEquals(binding.operation_uuid,
                                    entry.operation_uuid)) {
    return {TypeOperationRegistryStatus::sblr_binding_operation_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(binding.operation_family_uuid,
                                    entry.operation_family_uuid)) {
    return {TypeOperationRegistryStatus::sblr_binding_family_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.implementation_version != entry.implementation_version) {
    return {TypeOperationRegistryStatus::
                sblr_binding_implementation_version_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.schema_epoch != entry.schema_epoch) {
    return {TypeOperationRegistryStatus::sblr_binding_schema_epoch_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.security_epoch != entry.security_epoch) {
    return {TypeOperationRegistryStatus::sblr_binding_security_epoch_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.resource_epoch != entry.resource_epoch) {
    return {TypeOperationRegistryStatus::sblr_binding_resource_epoch_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.definition_hash != entry.definition_hash) {
    return {TypeOperationRegistryStatus::sblr_binding_definition_hash_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.argument_descriptor_uuids.size() !=
          entry.overload.argument_descriptor_uuids.size() ||
      binding.argument_domain_uuids.size() !=
          entry.overload.argument_domain_uuids.size()) {
    return {TypeOperationRegistryStatus::sblr_binding_argument_count_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationRegistryUuidVectorEquals(
          binding.argument_descriptor_uuids,
          entry.overload.argument_descriptor_uuids) ||
      !TypeOperationRegistryUuidVectorEquals(
          binding.argument_domain_uuids,
          entry.overload.argument_domain_uuids)) {
    return {TypeOperationRegistryStatus::sblr_binding_argument_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(binding.result_descriptor_uuid,
                                    entry.overload.result_descriptor_uuid) ||
      !ExecutionDataPacketUuidEquals(binding.result_domain_uuid,
                                    entry.overload.result_domain_uuid)) {
    return {TypeOperationRegistryStatus::sblr_binding_result_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (binding.diagnostic_search_key.empty()) {
    return {TypeOperationRegistryStatus::sblr_binding_diagnostic_key_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!binding.runtime_status_recheck || !binding.runtime_security_recheck ||
      !binding.runtime_resource_epoch_recheck ||
      !binding.runtime_implementation_version_recheck ||
      !binding.runtime_definition_hash_recheck) {
    return {TypeOperationRegistryStatus::sblr_binding_recheck_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!binding.source_sql_diagnostic_only) {
    return {TypeOperationRegistryStatus::sblr_source_sql_not_diagnostic_only,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult ValidateTypeOperationCacheKey(
    const TypeOperationRegistryEntry& entry,
    std::size_t entry_index) {
  if (!entry.cache_key_present) {
    return {TypeOperationRegistryStatus::cache_key_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  const auto& cache_key = entry.cache_key;
  if (!ExecutionDataPacketUuidEquals(cache_key.operation_uuid,
                                    entry.operation_uuid)) {
    return {TypeOperationRegistryStatus::cache_key_operation_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(cache_key.operation_family_uuid,
                                    entry.operation_family_uuid)) {
    return {TypeOperationRegistryStatus::cache_key_family_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (cache_key.schema_epoch != entry.schema_epoch ||
      cache_key.security_epoch != entry.security_epoch ||
      cache_key.resource_epoch != entry.resource_epoch) {
    return {TypeOperationRegistryStatus::cache_key_epoch_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (cache_key.implementation_version != entry.implementation_version) {
    return {TypeOperationRegistryStatus::
                cache_key_implementation_version_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (cache_key.definition_hash != entry.definition_hash) {
    return {TypeOperationRegistryStatus::cache_key_definition_hash_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationRegistryUuidVectorEquals(
          cache_key.argument_descriptor_uuids,
          entry.overload.argument_descriptor_uuids) ||
      !TypeOperationRegistryUuidVectorEquals(
          cache_key.argument_domain_uuids,
          entry.overload.argument_domain_uuids)) {
    return {TypeOperationRegistryStatus::cache_key_argument_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(cache_key.result_descriptor_uuid,
                                    entry.overload.result_descriptor_uuid) ||
      !ExecutionDataPacketUuidEquals(cache_key.result_domain_uuid,
                                    entry.overload.result_domain_uuid)) {
    return {TypeOperationRegistryStatus::cache_key_result_uuid_mismatch,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (TypeOperationImplementationTargetRequiresCppUdr(
          entry.implementation_target) &&
      (ExecutionDataPacketUuidIsNil(cache_key.cpp_udr_package_uuid) ||
       cache_key.cpp_udr_package_version == 0)) {
    return {TypeOperationRegistryStatus::cache_key_cpp_udr_missing,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (TypeOperationImplementationTargetRequiresLlvm(
          entry.implementation_target) &&
      (ExecutionDataPacketUuidIsNil(cache_key.llvm_artifact_uuid) ||
       cache_key.llvm_artifact_version == 0)) {
    return {TypeOperationRegistryStatus::cache_key_llvm_missing,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.reference_method_binding_present &&
      ExecutionDataPacketUuidIsNil(cache_key.reference_profile_uuid)) {
    return {TypeOperationRegistryStatus::cache_key_reference_profile_missing,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  return {};
}

inline TypeOperationRegistryValidationResult
ValidateTypeOperationRegistryEntry(const TypeOperationRegistryEntry& entry,
                                   std::size_t entry_index) {
  if (ExecutionDataPacketUuidIsNil(entry.operation_uuid)) {
    return {TypeOperationRegistryStatus::operation_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(entry.operation_family_uuid)) {
    return {TypeOperationRegistryStatus::operation_family_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(entry.schema_uuid)) {
    return {TypeOperationRegistryStatus::schema_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(entry.owning_package_uuid)) {
    return {TypeOperationRegistryStatus::owning_package_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(entry.name_ref_uuid)) {
    return {TypeOperationRegistryStatus::name_ref_uuid_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.operation_epoch == 0) {
    return {TypeOperationRegistryStatus::operation_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.schema_epoch == 0) {
    return {TypeOperationRegistryStatus::schema_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.security_epoch == 0) {
    return {TypeOperationRegistryStatus::security_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.resource_epoch == 0) {
    return {TypeOperationRegistryStatus::resource_epoch_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.implementation_version == 0) {
    return {TypeOperationRegistryStatus::implementation_version_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.definition_hash.empty()) {
    return {TypeOperationRegistryStatus::definition_hash_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.diagnostic_search_key.empty()) {
    return {TypeOperationRegistryStatus::diagnostic_search_key_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.conformance_key.empty()) {
    return {TypeOperationRegistryStatus::conformance_key_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.stable_name.empty()) {
    return {TypeOperationRegistryStatus::entry_stable_name_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.descriptor_authoritative) {
    return {TypeOperationRegistryStatus::entry_descriptor_not_authoritative,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.parser_independent) {
    return {TypeOperationRegistryStatus::entry_descriptor_parser_dependent,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationKindIsValid(entry.operation_kind)) {
    return {TypeOperationRegistryStatus::operation_kind_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationRegistryRowStatusIsValid(entry.row_status)) {
    return {TypeOperationRegistryStatus::row_status_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationImplementationTargetIsValid(entry.implementation_target)) {
    return {TypeOperationRegistryStatus::implementation_target_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.implementation_target_present &&
      entry.row_status != TypeOperationRegistryRowStatus::deferred &&
      entry.row_status != TypeOperationRegistryRowStatus::render_only &&
      entry.row_status !=
          TypeOperationRegistryRowStatus::unsupported_by_policy) {
    return {TypeOperationRegistryStatus::implementation_target_missing,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.implementation_target_present &&
      entry.implementation_target == TypeOperationImplementationTarget::none &&
      TypeOperationRegistryRowStatusIsExecutable(entry.row_status)) {
    return {TypeOperationRegistryStatus::implementation_target_missing,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.non_cpp_udr_runtime_requested) {
    return {TypeOperationRegistryStatus::non_cpp_udr_forbidden,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (TypeOperationRegistryRowStatusRequiresDiagnostic(entry.row_status) &&
      !entry.status_execution_refusal_diagnostic) {
    return {TypeOperationRegistryStatus::status_refusal_diagnostic_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.row_status == TypeOperationRegistryRowStatus::proposed &&
      ExecutionDataPacketUuidIsNil(entry.owner_transaction_uuid)) {
    return {TypeOperationRegistryStatus::proposed_owner_transaction_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (entry.row_status == TypeOperationRegistryRowStatus::proposed &&
      entry.sblr_binding.executable_outside_owner_transaction) {
    return {TypeOperationRegistryStatus::proposed_row_executable_outside_owner,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.transactional_registration) {
    return {TypeOperationRegistryStatus::transactional_registration_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.rollback_safe) {
    return {TypeOperationRegistryStatus::rollback_safe_registration_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.dependency_invalidation_registered) {
    return {TypeOperationRegistryStatus::dependency_invalidation_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.cache_invalidation_registered) {
    return {TypeOperationRegistryStatus::cache_invalidation_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.idempotent_definition_hash_checked) {
    return {TypeOperationRegistryStatus::idempotent_definition_hash_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!TypeOperationDomainStackPolicyIsValid(entry.domain_stack_policy)) {
    return {TypeOperationRegistryStatus::domain_stack_policy_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!DomainNullPolicyIsValid(entry.null_policy)) {
    return {TypeOperationRegistryStatus::null_policy_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!DomainMissingPolicyIsValid(entry.missing_policy)) {
    return {TypeOperationRegistryStatus::missing_policy_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (!entry.default_value_policy_declared ||
      !entry.unknown_value_policy_declared ||
      !entry.error_value_policy_declared) {
    return {TypeOperationRegistryStatus::value_policy_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }

  const auto overload_result =
      ValidateTypeOperationOverloadDescriptor(entry.overload, entry_index);
  if (!overload_result.ok()) {
    return overload_result;
  }

  if (!TypeOperationCastClassIsValid(entry.cast_class)) {
    return {TypeOperationRegistryStatus::cast_class_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if ((entry.operation_kind == TypeOperationKind::type_cast ||
       entry.operation_kind == TypeOperationKind::domain_cast) &&
      entry.cast_class == TypeOperationCastClass::none) {
    return {TypeOperationRegistryStatus::cast_class_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  if (TypeOperationKindRequiresDomainCast(entry.operation_kind)) {
    if (!entry.domain_cast_rule_present) {
      return {TypeOperationRegistryStatus::domain_cast_descriptor_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    const auto cast_result =
        ValidateDomainCastRuleDescriptor(entry.domain_cast_rule);
    if (!cast_result.ok()) {
      return {TypeOperationRegistryStatus::domain_cast_descriptor_invalid,
              cast_result.descriptor_status, cast_result.status,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (!ExecutionDataPacketUuidEquals(
            entry.domain_cast_rule_uuid,
            entry.domain_cast_rule.cast_rule_uuid)) {
      return {TypeOperationRegistryStatus::domain_cast_uuid_mismatch,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
  }
  if (TypeOperationKindRequiresDomainOperation(entry.operation_kind)) {
    if (!entry.domain_operation_present) {
      return {TypeOperationRegistryStatus::
                  domain_operation_descriptor_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    const auto operation_result =
        ValidateDomainOperationDescriptor(entry.domain_operation);
    if (!operation_result.ok()) {
      return {TypeOperationRegistryStatus::domain_operation_descriptor_invalid,
              operation_result.descriptor_status, DomainCastRuleStatus::ok,
              operation_result.status, entry_index,
              operation_result.operand_index};
    }
    if (!ExecutionDataPacketUuidEquals(
            entry.domain_operation_uuid,
            entry.domain_operation.operation_uuid)) {
      return {TypeOperationRegistryStatus::domain_operation_uuid_mismatch,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
  }
  if (TypeOperationKindRequiresAggregateState(entry.operation_kind)) {
    if (!entry.aggregate_state_present) {
      return {TypeOperationRegistryStatus::aggregate_state_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    const auto state_result = ValidateAggregateStateDescriptorForRegistry(
        entry.aggregate_state, entry_index);
    if (!state_result.ok()) {
      return state_result;
    }
  }
  if (TypeOperationKindRequiresWindowFrame(entry.operation_kind)) {
    if (!entry.window_frame_present) {
      return {TypeOperationRegistryStatus::window_frame_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    const auto frame_result =
        ValidateWindowFrameDescriptorForRegistry(entry.window_frame,
                                                 entry_index);
    if (!frame_result.ok()) {
      return frame_result;
    }
  }

  if (TypeOperationImplementationTargetRequiresCppUdr(
          entry.implementation_target) ||
      entry.row_status == TypeOperationRegistryRowStatus::bridge_only) {
    const auto hook_result =
        ValidateTypeOperationCppUdrHook(entry.cpp_udr_hook, entry_index);
    if (!hook_result.ok()) {
      return hook_result;
    }
    if (!entry.cpp_udr_bridge_loaded || !entry.cpp_udr_bridge_admitted ||
        ExecutionDataPacketUuidIsNil(entry.cpp_udr_package_uuid) ||
        entry.cpp_udr_package_version == 0) {
      return {TypeOperationRegistryStatus::cpp_udr_bridge_admission_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
  }
  if (TypeOperationImplementationTargetRequiresLlvm(
          entry.implementation_target)) {
    if (ExecutionDataPacketUuidIsNil(entry.llvm_artifact_uuid)) {
      return {TypeOperationRegistryStatus::llvm_artifact_uuid_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (entry.llvm_artifact_epoch == 0) {
      return {TypeOperationRegistryStatus::llvm_artifact_epoch_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (entry.llvm_artifact_version == 0) {
      return {TypeOperationRegistryStatus::llvm_artifact_version_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (ExecutionDataPacketUuidIsNil(entry.llvm_fallback_reference_uuid)) {
      return {TypeOperationRegistryStatus::llvm_fallback_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (!entry.llvm_fallback_matches) {
      return {TypeOperationRegistryStatus::llvm_fallback_mismatch,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (!entry.llvm_invalidates_with_artifact) {
      return {TypeOperationRegistryStatus::llvm_invalidation_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
  }

  if (TypeOperationRegistryRowStatusIsExecutable(entry.row_status)) {
    const auto sblr_result =
        ValidateTypeOperationSblrBinding(entry, entry_index);
    if (!sblr_result.ok()) {
      return sblr_result;
    }
  }

  const auto cache_result =
      ValidateTypeOperationCacheKey(entry, entry_index);
  if (!cache_result.ok()) {
    return cache_result;
  }

  if (entry.reference_method_binding_present &&
      (ExecutionDataPacketUuidIsNil(entry.reference_profile_uuid) ||
       entry.reference_family.empty() || entry.reference_version_profile.empty() ||
       entry.reference_method_name.empty() ||
       entry.inverse_rendering_policy.empty())) {
    return {TypeOperationRegistryStatus::reference_method_binding_invalid,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }

  if (TypeOperationRegistryRowStatusRequiresDiagnostic(entry.row_status) &&
      entry.diagnostics.empty()) {
    return {TypeOperationRegistryStatus::diagnostic_vector_required,
            ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
            DomainOperationDescriptorStatus::ok, entry_index, 0};
  }
  for (const auto& diagnostic : entry.diagnostics) {
    if (diagnostic.diagnostic_code.empty()) {
      return {TypeOperationRegistryStatus::diagnostic_code_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (!diagnostic.redaction_state_declared) {
      return {TypeOperationRegistryStatus::
                  diagnostic_redaction_state_required,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
  }

  return {};
}

inline bool TypeOperationRegistryHasDuplicateOperationUuid(
    const TypeOperationRegistry& registry,
    std::size_t entry_index) noexcept {
  for (std::size_t index = 0; index < entry_index; ++index) {
    if (ExecutionDataPacketUuidEquals(
            registry.entries[index].operation_uuid,
            registry.entries[entry_index].operation_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool TypeOperationRegistryHasDuplicateOverloadSignature(
    const TypeOperationRegistry& registry,
    std::size_t entry_index) noexcept {
  const auto& entry = registry.entries[entry_index];
  for (std::size_t index = 0; index < entry_index; ++index) {
    const auto& candidate = registry.entries[index];
    if (candidate.operation_kind == entry.operation_kind &&
        candidate.stable_name == entry.stable_name &&
        candidate.overload.overload_key == entry.overload.overload_key) {
      return true;
    }
  }
  return false;
}

inline TypeOperationRegistryValidationResult ValidateTypeOperationRegistry(
    const TypeOperationRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {TypeOperationRegistryStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {TypeOperationRegistryStatus::registry_epoch_required};
  }
  if (registry.stable_name.empty()) {
    return {TypeOperationRegistryStatus::stable_name_required};
  }
  if (ExecutionDataPacketUuidIsNil(registry.catalog_snapshot_uuid)) {
    return {TypeOperationRegistryStatus::catalog_snapshot_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(registry.visible_transaction_uuid)) {
    return {TypeOperationRegistryStatus::visible_transaction_uuid_required};
  }
  if (!registry.descriptor_authoritative) {
    return {TypeOperationRegistryStatus::descriptor_not_authoritative};
  }
  if (!registry.parser_independent) {
    return {TypeOperationRegistryStatus::descriptor_parser_dependent};
  }
  if (!registry.committed_catalog_visible) {
    return {TypeOperationRegistryStatus::committed_catalog_not_visible};
  }
  if (registry.entries.empty()) {
    return {TypeOperationRegistryStatus::entries_required};
  }
  if (!registry.local_metrics_root_declared) {
    return {TypeOperationRegistryStatus::local_metrics_root_required};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.type_operation.registration_count",
      "sys.metrics.type_operation.registration_failure_count",
      "sys.metrics.type_operation.active_count",
      "sys.metrics.type_operation.deferred_count",
      "sys.metrics.type_operation.unsupported_count",
      "sys.metrics.type_operation.execution_count",
      "sys.metrics.type_operation.execution_refusal_count",
      "sys.metrics.type_operation.overload_resolution_count",
      "sys.metrics.type_operation.overload_ambiguous_count",
      "sys.metrics.type_operation.cast_execution_count",
      "sys.metrics.type_operation.cast_refusal_count",
      "sys.metrics.type_operation.sblr_stale_binding_count",
      "sys.metrics.type_operation.udr_bridge_invocation_count",
      "sys.metrics.type_operation.non_cpp_udr_refusal_count",
      "sys.metrics.type_operation.llvm_fallback_count",
      "sys.metrics.type_operation.cache_invalidation_count"};
  for (std::string_view metric : required_metrics) {
    if (!TypeOperationRegistryMetricPresent(registry, metric)) {
      return {TypeOperationRegistryStatus::local_metric_missing};
    }
  }
  if (!registry.cluster_metrics_guarded_by_cluster_governance) {
    return {TypeOperationRegistryStatus::cluster_metrics_guard_required};
  }

  for (std::size_t entry_index = 0; entry_index < registry.entries.size();
       ++entry_index) {
    if (TypeOperationRegistryHasDuplicateOperationUuid(registry,
                                                       entry_index)) {
      return {TypeOperationRegistryStatus::duplicate_operation_uuid,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    if (TypeOperationRegistryHasDuplicateOverloadSignature(registry,
                                                           entry_index)) {
      return {TypeOperationRegistryStatus::duplicate_overload_signature,
              ExecutionDataPacketStatus::ok, DomainCastRuleStatus::ok,
              DomainOperationDescriptorStatus::ok, entry_index, 0};
    }
    const auto entry_result =
        ValidateTypeOperationRegistryEntry(registry.entries[entry_index],
                                           entry_index);
    if (!entry_result.ok()) {
      return entry_result;
    }
  }

  return {};
}

// SEARCH_KEY: TIO-TYPE-INDEX-STATS-OPTIMIZER
// SEARCH_KEY: TIO-CANONICAL-COMPARISON-CONTRACT
// SEARCH_KEY: TIO-CANONICALIZATION-PROFILE
// SEARCH_KEY: TIO-INDEX-COMPATIBILITY-DESCRIPTOR
// SEARCH_KEY: TIO-INDEX-FAMILY-MATRIX
// SEARCH_KEY: TIO-TYPE-STATISTICS-DESCRIPTOR
// SEARCH_KEY: TIO-SELECTIVITY-MODEL
// SEARCH_KEY: TIO-OPTIMIZER-ADMISSION
enum class TypeIndexFamily : std::uint8_t {
  btree = 0,
  hash = 1,
  bitmap = 2,
  inverted = 3,
  full_text = 4,
  spatial = 5,
  vector = 6,
  columnar = 7,
  bloom = 8,
  sketch = 9,
  graph = 10,
  time_series = 11,
  extension = 12
};

constexpr bool TypeIndexFamilyIsValid(TypeIndexFamily family) noexcept {
  switch (family) {
    case TypeIndexFamily::btree:
    case TypeIndexFamily::hash:
    case TypeIndexFamily::bitmap:
    case TypeIndexFamily::inverted:
    case TypeIndexFamily::full_text:
    case TypeIndexFamily::spatial:
    case TypeIndexFamily::vector:
    case TypeIndexFamily::columnar:
    case TypeIndexFamily::bloom:
    case TypeIndexFamily::sketch:
    case TypeIndexFamily::graph:
    case TypeIndexFamily::time_series:
    case TypeIndexFamily::extension:
      return true;
  }
  return false;
}

enum class TypeCanonicalizationProfileFamily : std::uint8_t {
  scalar = 0,
  text = 1,
  numeric = 2,
  temporal = 3,
  range = 4,
  document = 5,
  spatial = 6,
  vector = 7,
  enum_set = 8,
  opaque = 9
};

constexpr bool TypeCanonicalizationProfileFamilyIsValid(
    TypeCanonicalizationProfileFamily family) noexcept {
  switch (family) {
    case TypeCanonicalizationProfileFamily::scalar:
    case TypeCanonicalizationProfileFamily::text:
    case TypeCanonicalizationProfileFamily::numeric:
    case TypeCanonicalizationProfileFamily::temporal:
    case TypeCanonicalizationProfileFamily::range:
    case TypeCanonicalizationProfileFamily::document:
    case TypeCanonicalizationProfileFamily::spatial:
    case TypeCanonicalizationProfileFamily::vector:
    case TypeCanonicalizationProfileFamily::enum_set:
    case TypeCanonicalizationProfileFamily::opaque:
      return true;
  }
  return false;
}

enum class TypeIndexLossinessPolicy : std::uint8_t {
  exact = 0,
  lossy_requires_recheck = 1,
  unsupported = 2
};

constexpr bool TypeIndexLossinessPolicyIsValid(
    TypeIndexLossinessPolicy policy) noexcept {
  switch (policy) {
    case TypeIndexLossinessPolicy::exact:
    case TypeIndexLossinessPolicy::lossy_requires_recheck:
    case TypeIndexLossinessPolicy::unsupported:
      return true;
  }
  return false;
}

enum class TypeIndexFamilySupport : std::uint8_t {
  supported = 0,
  supported_with_recheck = 1,
  conservative = 2,
  unsupported = 3,
  deferred = 4
};

constexpr bool TypeIndexFamilySupportIsValid(
    TypeIndexFamilySupport support) noexcept {
  switch (support) {
    case TypeIndexFamilySupport::supported:
    case TypeIndexFamilySupport::supported_with_recheck:
    case TypeIndexFamilySupport::conservative:
    case TypeIndexFamilySupport::unsupported:
    case TypeIndexFamilySupport::deferred:
      return true;
  }
  return false;
}

enum class TypeStatisticsSampleScope : std::uint8_t {
  table = 0,
  index = 1,
  materialized_result = 2,
  shard = 3,
  filespace = 4,
  relation_source = 5,
  cluster_route = 6
};

constexpr bool TypeStatisticsSampleScopeIsValid(
    TypeStatisticsSampleScope scope) noexcept {
  switch (scope) {
    case TypeStatisticsSampleScope::table:
    case TypeStatisticsSampleScope::index:
    case TypeStatisticsSampleScope::materialized_result:
    case TypeStatisticsSampleScope::shard:
    case TypeStatisticsSampleScope::filespace:
    case TypeStatisticsSampleScope::relation_source:
    case TypeStatisticsSampleScope::cluster_route:
      return true;
  }
  return false;
}

enum class TypeSelectivityUnknownPolicy : std::uint8_t {
  refuse = 0,
  explicit_unknown = 1,
  conservative_estimate = 2
};

constexpr bool TypeSelectivityUnknownPolicyIsValid(
    TypeSelectivityUnknownPolicy policy) noexcept {
  switch (policy) {
    case TypeSelectivityUnknownPolicy::refuse:
    case TypeSelectivityUnknownPolicy::explicit_unknown:
    case TypeSelectivityUnknownPolicy::conservative_estimate:
      return true;
  }
  return false;
}

enum class TypeOptimizerAdmissionState : std::uint8_t {
  admitted = 0,
  admitted_conservative = 1,
  refused_unknown_semantics = 2,
  refused_security = 3,
  refused_epoch_mismatch = 4,
  refused_unsupported = 5,
  requires_recheck = 6,
  deferred = 7
};

constexpr bool TypeOptimizerAdmissionStateIsValid(
    TypeOptimizerAdmissionState state) noexcept {
  switch (state) {
    case TypeOptimizerAdmissionState::admitted:
    case TypeOptimizerAdmissionState::admitted_conservative:
    case TypeOptimizerAdmissionState::refused_unknown_semantics:
    case TypeOptimizerAdmissionState::refused_security:
    case TypeOptimizerAdmissionState::refused_epoch_mismatch:
    case TypeOptimizerAdmissionState::refused_unsupported:
    case TypeOptimizerAdmissionState::requires_recheck:
    case TypeOptimizerAdmissionState::deferred:
      return true;
  }
  return false;
}

struct CanonicalComparisonContract {
  Uuid comparison_contract_uuid{};
  Uuid descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionTypeDescriptor descriptor;
  Uuid equality_operation_uuid{};
  Uuid ordering_operation_uuid{};
  Uuid hash_operation_uuid{};
  Uuid canonicalization_profile_uuid{};
  ExecutionComparisonNullOrdering null_order_policy =
      ExecutionComparisonNullOrdering::nulls_last;
  DomainMissingPolicy missing_policy = DomainMissingPolicy::propagate_missing;
  Uuid collation_uuid{};
  std::string timezone_policy;
  std::uint64_t resource_epoch = 0;
  Uuid reference_profile_uuid{};
  std::vector<TypeIndexFamily> index_equivalence_class;
  bool supports_equality = true;
  bool supports_ordering = false;
  bool supports_hashing = false;
  bool supports_grouping = true;
  bool supports_distinct = true;
  bool supports_join = true;
  bool supports_index = false;
  bool hash_consistent_with_equality = true;
  bool ordering_consistent_with_ordered_index = true;
  bool grouping_consistent_with_equality = true;
  bool reference_policy_declared = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct CanonicalizationProfile {
  Uuid canonicalization_profile_uuid{};
  Uuid descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionTypeDescriptor descriptor;
  TypeCanonicalizationProfileFamily profile_family =
      TypeCanonicalizationProfileFamily::scalar;
  Uuid resource_profile_uuid{};
  std::uint64_t resource_epoch = 0;
  std::uint64_t profile_version = 0;
  bool storage_normalization_declared = true;
  bool comparison_normalization_declared = true;
  bool hash_normalization_declared = true;
  bool index_normalization_declared = true;
  bool text_unicode_normalization_declared = false;
  bool text_case_accent_padding_declared = false;
  bool text_collation_key_declared = false;
  bool text_charset_validation_declared = false;
  bool numeric_scale_decimal_context_declared = false;
  bool numeric_special_value_posture_declared = false;
  bool numeric_overflow_declared = false;
  bool temporal_instant_display_declared = false;
  bool temporal_timezone_precision_declared = false;
  bool temporal_calendar_edge_declared = false;
  bool range_bound_canonicalization_declared = false;
  bool range_merge_adjacency_declared = false;
  bool document_key_order_declared = false;
  bool document_missing_null_declared = false;
  bool spatial_reference_declared = false;
  bool spatial_validity_declared = false;
  bool vector_shape_declared = false;
  bool vector_metric_declared = false;
  bool enum_label_policy_declared = false;
  bool enum_unknown_label_declared = false;
  bool opaque_cpp_udr_canonicalizer_present = false;
  bool opaque_explicit_refusal = false;
  DomainCppUdrOperationHook opaque_cpp_udr_hook;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct IndexCompatibilityDescriptor {
  Uuid index_compatibility_uuid{};
  Uuid descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionTypeDescriptor descriptor;
  Uuid comparison_contract_uuid{};
  TypeIndexFamily index_family = TypeIndexFamily::btree;
  Uuid key_encoding_profile_uuid{};
  std::vector<Uuid> predicate_operation_refs;
  bool covering_values_allowed = false;
  bool covering_value_policy_declared = true;
  bool partial_index_allowed = false;
  bool partial_index_policy_declared = true;
  bool expression_index_allowed = false;
  bool expression_index_policy_declared = true;
  bool resource_dependencies_declared = true;
  bool rebuild_required_on_change = true;
  bool descriptor_change_rebuild_required = true;
  bool resource_change_rebuild_required = true;
  TypeIndexLossinessPolicy lossiness_policy = TypeIndexLossinessPolicy::exact;
  bool exact_recheck_required = false;
  Uuid reference_profile_uuid{};
  bool reference_compatibility_declared = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct TypeIndexFamilyMatrixClassification {
  TypeIndexFamily index_family = TypeIndexFamily::btree;
  TypeIndexFamilySupport support = TypeIndexFamilySupport::unsupported;
  bool diagnostic_declared = true;
};

struct TypeIndexFamilyMatrixRow {
  ExecutionTypeFamily type_family = ExecutionTypeFamily::unknown;
  bool domain_family = false;
  std::vector<TypeIndexFamilyMatrixClassification> classifications;
};

struct TypeMostCommonValueSummary {
  Uuid value_summary_uuid{};
  ExecutionValueState value_state = ExecutionValueState::value;
  std::uint64_t frequency = 0;
  bool descriptor_safe_summary = true;
  bool rare_value_redacted = true;
};

struct TypeHistogramSummary {
  Uuid histogram_uuid{};
  std::string histogram_kind;
  std::uint32_t bucket_count = 0;
  bool descriptor_specific = true;
  bool resource_epoch_bound = true;
  bool protected_values_redacted = true;
};

struct TypeStatisticsDescriptor {
  Uuid statistics_uuid{};
  Uuid descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionTypeDescriptor descriptor;
  Uuid source_object_uuid{};
  TypeStatisticsSampleScope sample_scope = TypeStatisticsSampleScope::table;
  std::uint64_t schema_epoch = 0;
  std::uint64_t sample_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t row_count = 0;
  std::uint64_t null_count = 0;
  std::uint64_t missing_count = 0;
  std::uint64_t distinct_count = 0;
  bool null_count_declared = true;
  bool missing_count_declared = true;
  bool distinct_count_declared = true;
  std::vector<TypeMostCommonValueSummary> most_common_values;
  TypeHistogramSummary histogram;
  bool histogram_present = true;
  bool correlation_declared = true;
  bool element_path_stats_required = false;
  std::vector<DomainElementPath> element_path_stats;
  Uuid selectivity_model_uuid{};
  Uuid privacy_policy_uuid{};
  bool privacy_policy_enforced = true;
  bool protected_rare_values_redacted = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct TypeSelectivityModel {
  Uuid selectivity_model_uuid{};
  Uuid descriptor_uuid{};
  Uuid operation_uuid{};
  Uuid statistics_uuid{};
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  TypeSelectivityUnknownPolicy unknown_policy =
      TypeSelectivityUnknownPolicy::explicit_unknown;
  bool descriptor_compatible = true;
  bool rejects_descriptor_mismatch = true;
  bool rejects_resource_mismatch = true;
  bool rejects_security_mismatch = true;
  bool protected_value_leak_prevented = true;
  bool unknown_selectivity_explicit = true;
  bool non_cpp_udr_metadata_requested = false;
  bool cpp_udr_metadata_trusted = true;
  DomainCppUdrOperationHook cpp_udr_metadata_hook;
};

struct TypeOptimizerCacheKeyDescriptor {
  Uuid database_uuid{};
  Uuid query_scope_uuid{};
  Uuid normalized_sblr_expression_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  std::string domain_stack_hash;
  Uuid operation_uuid{};
  Uuid comparison_contract_uuid{};
  Uuid canonicalization_profile_uuid{};
  Uuid index_compatibility_uuid{};
  Uuid index_uuid{};
  std::uint64_t index_generation = 0;
  Uuid statistics_uuid{};
  std::uint64_t sample_epoch = 0;
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  Uuid reference_profile_uuid{};
  Uuid parser_family_uuid{};
  std::string plan_cache_key_hash;
  bool includes_descriptor = true;
  bool includes_domain_when_present = true;
  bool includes_operation = true;
  bool includes_comparison_when_present = true;
  bool includes_canonicalization_when_present = true;
  bool includes_index_when_present = true;
  bool includes_statistics_when_present = true;
  bool includes_schema_epoch = true;
  bool includes_security_epoch = true;
  bool includes_resource_epoch = true;
  bool includes_reference_when_present = true;
  bool parser_family_untrusted_context_only = true;
};

struct TypeOptimizerDiagnosticVector {
  std::string diagnostic_code;
  Uuid descriptor_uuid{};
  Uuid operation_uuid{};
  Uuid comparison_contract_uuid{};
  Uuid index_compatibility_uuid{};
  Uuid statistics_uuid{};
  std::uint64_t resource_epoch = 0;
  std::uint64_t security_epoch = 0;
  Uuid reference_profile_uuid{};
  bool conservative_planning_allowed = false;
  bool redaction_state_declared = true;
};

struct TypeOptimizerDecisionInput {
  Uuid decision_uuid{};
  Uuid query_scope_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  Uuid operation_uuid{};
  Uuid comparison_contract_uuid{};
  Uuid canonicalization_profile_uuid{};
  Uuid index_compatibility_uuid{};
  Uuid statistics_uuid{};
  Uuid selectivity_model_uuid{};
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  Uuid reference_profile_uuid{};
  TypeOptimizerAdmissionState admission_result =
      TypeOptimizerAdmissionState::admitted;
  ExecutionTypeDescriptor descriptor;
  bool descriptor_visible = true;
  bool operation_visible = true;
  bool operation_executable = true;
  TypeOperationRegistryEntry operation_entry;
  bool operation_entry_present = true;
  bool requires_comparison = true;
  CanonicalComparisonContract comparison_contract;
  bool comparison_contract_present = true;
  bool requires_canonicalization = true;
  CanonicalizationProfile canonicalization_profile;
  bool canonicalization_profile_present = true;
  bool considers_index = true;
  IndexCompatibilityDescriptor index_compatibility;
  bool index_compatibility_present = true;
  bool considers_statistics = true;
  TypeStatisticsDescriptor statistics;
  bool statistics_present = true;
  TypeSelectivityModel selectivity_model;
  bool selectivity_model_present = true;
  bool conservative_estimation_allowed = false;
  bool security_policy_permits = true;
  bool uses_protected_statistics = false;
  bool reference_compatibility_required = false;
  bool reference_semantics_preserved = true;
  bool selected_lossy_index = false;
  bool exact_recheck_obligation_present = false;
  bool udr_metadata_requested = false;
  bool udr_metadata_cpp = true;
  bool udr_metadata_trusted = true;
  TypeOptimizerCacheKeyDescriptor cache_key;
  bool cache_key_present = true;
  TypeOptimizerDiagnosticVector diagnostic;
  bool diagnostic_present = true;
};

struct TypeIndexStatsOptimizerContract {
  Uuid contract_uuid{};
  std::uint64_t contract_epoch = 0;
  std::string stable_name;
  std::vector<TypeOptimizerDecisionInput> decisions;
  std::vector<TypeIndexFamilyMatrixRow> index_family_matrix;
  std::vector<std::string> local_metric_names;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool local_metrics_root_declared = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
};

enum class TypeIndexStatsOptimizerStatus : std::uint16_t {
  ok = 0,
  contract_uuid_required = 1,
  contract_epoch_required = 2,
  stable_name_required = 3,
  descriptor_not_authoritative = 4,
  descriptor_parser_dependent = 5,
  decisions_required = 6,
  decision_uuid_required = 7,
  query_scope_uuid_required = 8,
  descriptor_uuid_required = 9,
  operation_uuid_required = 10,
  schema_epoch_required = 11,
  security_epoch_required = 12,
  resource_epoch_required = 13,
  descriptor_invalid = 14,
  descriptor_uuid_mismatch = 15,
  descriptor_epoch_mismatch = 16,
  descriptor_not_visible = 17,
  operation_not_visible = 18,
  operation_not_executable = 19,
  operation_descriptor_required = 20,
  operation_descriptor_invalid = 21,
  operation_descriptor_uuid_mismatch = 22,
  comparison_contract_required = 23,
  comparison_contract_uuid_required = 24,
  comparison_descriptor_mismatch = 25,
  comparison_epoch_mismatch = 26,
  comparison_contract_invalid = 27,
  comparison_resource_epoch_mismatch = 28,
  comparison_equality_operation_required = 29,
  comparison_ordering_operation_required = 30,
  comparison_hash_operation_required = 31,
  comparison_hash_equality_mismatch = 32,
  comparison_grouping_equality_mismatch = 33,
  comparison_index_order_mismatch = 34,
  canonicalization_profile_required = 35,
  canonicalization_profile_uuid_required = 36,
  canonicalization_descriptor_mismatch = 37,
  canonicalization_epoch_mismatch = 38,
  canonicalization_profile_invalid = 39,
  canonicalization_resource_epoch_mismatch = 40,
  canonicalization_family_mismatch = 41,
  canonicalization_family_rules_missing = 42,
  index_compatibility_required = 43,
  index_compatibility_uuid_required = 44,
  index_descriptor_mismatch = 45,
  index_comparison_contract_mismatch = 46,
  index_family_invalid = 47,
  index_key_encoding_required = 48,
  index_predicate_operation_required = 49,
  index_policy_required = 50,
  index_resource_dependency_required = 51,
  index_rebuild_rule_required = 52,
  index_lossiness_invalid = 53,
  index_recheck_required = 54,
  statistics_required = 55,
  statistics_uuid_required = 56,
  statistics_descriptor_mismatch = 57,
  statistics_source_required = 58,
  statistics_scope_invalid = 59,
  statistics_epoch_required = 60,
  statistics_count_invalid = 61,
  statistics_privacy_required = 62,
  statistics_privacy_denied = 63,
  statistics_histogram_required = 64,
  statistics_mcv_required = 65,
  statistics_element_path_invalid = 66,
  selectivity_model_required = 67,
  selectivity_model_uuid_required = 68,
  selectivity_descriptor_mismatch = 69,
  selectivity_operation_mismatch = 70,
  selectivity_statistics_mismatch = 71,
  selectivity_epoch_mismatch = 72,
  selectivity_policy_invalid = 73,
  selectivity_unknown_not_explicit = 74,
  selectivity_incompatible_stats_not_rejected = 75,
  security_policy_refused = 76,
  reference_semantics_unsupported = 77,
  non_cpp_udr_metadata_forbidden = 78,
  udr_metadata_untrusted = 79,
  cache_key_required = 80,
  cache_key_uuid_required = 81,
  cache_key_dependency_missing = 82,
  cache_key_epoch_mismatch = 83,
  cache_key_parser_authority = 84,
  diagnostic_required = 85,
  diagnostic_code_required = 86,
  diagnostic_context_missing = 87,
  diagnostic_redaction_required = 88,
  admission_state_invalid = 89,
  unsafe_admission_result = 90,
  index_family_matrix_required = 91,
  index_family_matrix_family_missing = 92,
  index_family_matrix_family_duplicate = 93,
  index_family_matrix_classification_missing = 94,
  index_family_matrix_index_duplicate = 95,
  index_family_matrix_support_invalid = 96,
  index_family_matrix_diagnostic_required = 97,
  local_metrics_root_required = 98,
  local_metric_missing = 99,
  cluster_metrics_guard_required = 100
};

constexpr std::string_view TypeIndexStatsOptimizerStatusName(
    TypeIndexStatsOptimizerStatus status) noexcept {
  switch (status) {
    case TypeIndexStatsOptimizerStatus::ok:
      return "ok";
    case TypeIndexStatsOptimizerStatus::contract_uuid_required:
      return "contract_uuid_required";
    case TypeIndexStatsOptimizerStatus::contract_epoch_required:
      return "contract_epoch_required";
    case TypeIndexStatsOptimizerStatus::stable_name_required:
      return "stable_name_required";
    case TypeIndexStatsOptimizerStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case TypeIndexStatsOptimizerStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case TypeIndexStatsOptimizerStatus::decisions_required:
      return "decisions_required";
    case TypeIndexStatsOptimizerStatus::decision_uuid_required:
      return "decision_uuid_required";
    case TypeIndexStatsOptimizerStatus::query_scope_uuid_required:
      return "query_scope_uuid_required";
    case TypeIndexStatsOptimizerStatus::descriptor_uuid_required:
      return "descriptor_uuid_required";
    case TypeIndexStatsOptimizerStatus::operation_uuid_required:
      return "operation_uuid_required";
    case TypeIndexStatsOptimizerStatus::schema_epoch_required:
      return "schema_epoch_required";
    case TypeIndexStatsOptimizerStatus::security_epoch_required:
      return "security_epoch_required";
    case TypeIndexStatsOptimizerStatus::resource_epoch_required:
      return "resource_epoch_required";
    case TypeIndexStatsOptimizerStatus::descriptor_invalid:
      return "descriptor_invalid";
    case TypeIndexStatsOptimizerStatus::descriptor_uuid_mismatch:
      return "descriptor_uuid_mismatch";
    case TypeIndexStatsOptimizerStatus::descriptor_epoch_mismatch:
      return "descriptor_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::descriptor_not_visible:
      return "descriptor_not_visible";
    case TypeIndexStatsOptimizerStatus::operation_not_visible:
      return "operation_not_visible";
    case TypeIndexStatsOptimizerStatus::operation_not_executable:
      return "operation_not_executable";
    case TypeIndexStatsOptimizerStatus::operation_descriptor_required:
      return "operation_descriptor_required";
    case TypeIndexStatsOptimizerStatus::operation_descriptor_invalid:
      return "operation_descriptor_invalid";
    case TypeIndexStatsOptimizerStatus::operation_descriptor_uuid_mismatch:
      return "operation_descriptor_uuid_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_contract_required:
      return "comparison_contract_required";
    case TypeIndexStatsOptimizerStatus::comparison_contract_uuid_required:
      return "comparison_contract_uuid_required";
    case TypeIndexStatsOptimizerStatus::comparison_descriptor_mismatch:
      return "comparison_descriptor_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_epoch_mismatch:
      return "comparison_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_contract_invalid:
      return "comparison_contract_invalid";
    case TypeIndexStatsOptimizerStatus::comparison_resource_epoch_mismatch:
      return "comparison_resource_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_equality_operation_required:
      return "comparison_equality_operation_required";
    case TypeIndexStatsOptimizerStatus::comparison_ordering_operation_required:
      return "comparison_ordering_operation_required";
    case TypeIndexStatsOptimizerStatus::comparison_hash_operation_required:
      return "comparison_hash_operation_required";
    case TypeIndexStatsOptimizerStatus::comparison_hash_equality_mismatch:
      return "comparison_hash_equality_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_grouping_equality_mismatch:
      return "comparison_grouping_equality_mismatch";
    case TypeIndexStatsOptimizerStatus::comparison_index_order_mismatch:
      return "comparison_index_order_mismatch";
    case TypeIndexStatsOptimizerStatus::canonicalization_profile_required:
      return "canonicalization_profile_required";
    case TypeIndexStatsOptimizerStatus::canonicalization_profile_uuid_required:
      return "canonicalization_profile_uuid_required";
    case TypeIndexStatsOptimizerStatus::canonicalization_descriptor_mismatch:
      return "canonicalization_descriptor_mismatch";
    case TypeIndexStatsOptimizerStatus::canonicalization_epoch_mismatch:
      return "canonicalization_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::canonicalization_profile_invalid:
      return "canonicalization_profile_invalid";
    case TypeIndexStatsOptimizerStatus::
        canonicalization_resource_epoch_mismatch:
      return "canonicalization_resource_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::canonicalization_family_mismatch:
      return "canonicalization_family_mismatch";
    case TypeIndexStatsOptimizerStatus::canonicalization_family_rules_missing:
      return "canonicalization_family_rules_missing";
    case TypeIndexStatsOptimizerStatus::index_compatibility_required:
      return "index_compatibility_required";
    case TypeIndexStatsOptimizerStatus::index_compatibility_uuid_required:
      return "index_compatibility_uuid_required";
    case TypeIndexStatsOptimizerStatus::index_descriptor_mismatch:
      return "index_descriptor_mismatch";
    case TypeIndexStatsOptimizerStatus::index_comparison_contract_mismatch:
      return "index_comparison_contract_mismatch";
    case TypeIndexStatsOptimizerStatus::index_family_invalid:
      return "index_family_invalid";
    case TypeIndexStatsOptimizerStatus::index_key_encoding_required:
      return "index_key_encoding_required";
    case TypeIndexStatsOptimizerStatus::index_predicate_operation_required:
      return "index_predicate_operation_required";
    case TypeIndexStatsOptimizerStatus::index_policy_required:
      return "index_policy_required";
    case TypeIndexStatsOptimizerStatus::index_resource_dependency_required:
      return "index_resource_dependency_required";
    case TypeIndexStatsOptimizerStatus::index_rebuild_rule_required:
      return "index_rebuild_rule_required";
    case TypeIndexStatsOptimizerStatus::index_lossiness_invalid:
      return "index_lossiness_invalid";
    case TypeIndexStatsOptimizerStatus::index_recheck_required:
      return "index_recheck_required";
    case TypeIndexStatsOptimizerStatus::statistics_required:
      return "statistics_required";
    case TypeIndexStatsOptimizerStatus::statistics_uuid_required:
      return "statistics_uuid_required";
    case TypeIndexStatsOptimizerStatus::statistics_descriptor_mismatch:
      return "statistics_descriptor_mismatch";
    case TypeIndexStatsOptimizerStatus::statistics_source_required:
      return "statistics_source_required";
    case TypeIndexStatsOptimizerStatus::statistics_scope_invalid:
      return "statistics_scope_invalid";
    case TypeIndexStatsOptimizerStatus::statistics_epoch_required:
      return "statistics_epoch_required";
    case TypeIndexStatsOptimizerStatus::statistics_count_invalid:
      return "statistics_count_invalid";
    case TypeIndexStatsOptimizerStatus::statistics_privacy_required:
      return "statistics_privacy_required";
    case TypeIndexStatsOptimizerStatus::statistics_privacy_denied:
      return "statistics_privacy_denied";
    case TypeIndexStatsOptimizerStatus::statistics_histogram_required:
      return "statistics_histogram_required";
    case TypeIndexStatsOptimizerStatus::statistics_mcv_required:
      return "statistics_mcv_required";
    case TypeIndexStatsOptimizerStatus::statistics_element_path_invalid:
      return "statistics_element_path_invalid";
    case TypeIndexStatsOptimizerStatus::selectivity_model_required:
      return "selectivity_model_required";
    case TypeIndexStatsOptimizerStatus::selectivity_model_uuid_required:
      return "selectivity_model_uuid_required";
    case TypeIndexStatsOptimizerStatus::selectivity_descriptor_mismatch:
      return "selectivity_descriptor_mismatch";
    case TypeIndexStatsOptimizerStatus::selectivity_operation_mismatch:
      return "selectivity_operation_mismatch";
    case TypeIndexStatsOptimizerStatus::selectivity_statistics_mismatch:
      return "selectivity_statistics_mismatch";
    case TypeIndexStatsOptimizerStatus::selectivity_epoch_mismatch:
      return "selectivity_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::selectivity_policy_invalid:
      return "selectivity_policy_invalid";
    case TypeIndexStatsOptimizerStatus::selectivity_unknown_not_explicit:
      return "selectivity_unknown_not_explicit";
    case TypeIndexStatsOptimizerStatus::
        selectivity_incompatible_stats_not_rejected:
      return "selectivity_incompatible_stats_not_rejected";
    case TypeIndexStatsOptimizerStatus::security_policy_refused:
      return "security_policy_refused";
    case TypeIndexStatsOptimizerStatus::reference_semantics_unsupported:
      return "reference_semantics_unsupported";
    case TypeIndexStatsOptimizerStatus::non_cpp_udr_metadata_forbidden:
      return "non_cpp_udr_metadata_forbidden";
    case TypeIndexStatsOptimizerStatus::udr_metadata_untrusted:
      return "udr_metadata_untrusted";
    case TypeIndexStatsOptimizerStatus::cache_key_required:
      return "cache_key_required";
    case TypeIndexStatsOptimizerStatus::cache_key_uuid_required:
      return "cache_key_uuid_required";
    case TypeIndexStatsOptimizerStatus::cache_key_dependency_missing:
      return "cache_key_dependency_missing";
    case TypeIndexStatsOptimizerStatus::cache_key_epoch_mismatch:
      return "cache_key_epoch_mismatch";
    case TypeIndexStatsOptimizerStatus::cache_key_parser_authority:
      return "cache_key_parser_authority";
    case TypeIndexStatsOptimizerStatus::diagnostic_required:
      return "diagnostic_required";
    case TypeIndexStatsOptimizerStatus::diagnostic_code_required:
      return "diagnostic_code_required";
    case TypeIndexStatsOptimizerStatus::diagnostic_context_missing:
      return "diagnostic_context_missing";
    case TypeIndexStatsOptimizerStatus::diagnostic_redaction_required:
      return "diagnostic_redaction_required";
    case TypeIndexStatsOptimizerStatus::admission_state_invalid:
      return "admission_state_invalid";
    case TypeIndexStatsOptimizerStatus::unsafe_admission_result:
      return "unsafe_admission_result";
    case TypeIndexStatsOptimizerStatus::index_family_matrix_required:
      return "index_family_matrix_required";
    case TypeIndexStatsOptimizerStatus::index_family_matrix_family_missing:
      return "index_family_matrix_family_missing";
    case TypeIndexStatsOptimizerStatus::index_family_matrix_family_duplicate:
      return "index_family_matrix_family_duplicate";
    case TypeIndexStatsOptimizerStatus::
        index_family_matrix_classification_missing:
      return "index_family_matrix_classification_missing";
    case TypeIndexStatsOptimizerStatus::index_family_matrix_index_duplicate:
      return "index_family_matrix_index_duplicate";
    case TypeIndexStatsOptimizerStatus::index_family_matrix_support_invalid:
      return "index_family_matrix_support_invalid";
    case TypeIndexStatsOptimizerStatus::
        index_family_matrix_diagnostic_required:
      return "index_family_matrix_diagnostic_required";
    case TypeIndexStatsOptimizerStatus::local_metrics_root_required:
      return "local_metrics_root_required";
    case TypeIndexStatsOptimizerStatus::local_metric_missing:
      return "local_metric_missing";
    case TypeIndexStatsOptimizerStatus::cluster_metrics_guard_required:
      return "cluster_metrics_guard_required";
  }
  return "unknown_status";
}

struct TypeIndexStatsOptimizerValidationResult {
  TypeIndexStatsOptimizerStatus status = TypeIndexStatsOptimizerStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  TypeOperationRegistryStatus operation_status =
      TypeOperationRegistryStatus::ok;
  DomainElementPathStatus path_status = DomainElementPathStatus::ok;
  std::size_t decision_index = 0;
  std::size_t matrix_row_index = 0;

  bool ok() const noexcept {
    return status == TypeIndexStatsOptimizerStatus::ok;
  }
};

constexpr bool TypeIndexStatsOptimizerDescriptorFamilyRequiresProfile(
    ExecutionTypeFamily family) noexcept {
  switch (family) {
    case ExecutionTypeFamily::character:
    case ExecutionTypeFamily::decimal:
    case ExecutionTypeFamily::real:
    case ExecutionTypeFamily::temporal:
    case ExecutionTypeFamily::range:
    case ExecutionTypeFamily::document:
    case ExecutionTypeFamily::spatial:
    case ExecutionTypeFamily::vector:
    case ExecutionTypeFamily::structured:
    case ExecutionTypeFamily::opaque:
      return true;
    default:
      return false;
  }
}

constexpr TypeCanonicalizationProfileFamily
TypeIndexStatsOptimizerExpectedCanonicalizationFamily(
    ExecutionTypeFamily family) noexcept {
  switch (family) {
    case ExecutionTypeFamily::character:
      return TypeCanonicalizationProfileFamily::text;
    case ExecutionTypeFamily::signed_integer:
    case ExecutionTypeFamily::unsigned_integer:
    case ExecutionTypeFamily::real:
    case ExecutionTypeFamily::decimal:
      return TypeCanonicalizationProfileFamily::numeric;
    case ExecutionTypeFamily::temporal:
      return TypeCanonicalizationProfileFamily::temporal;
    case ExecutionTypeFamily::range:
      return TypeCanonicalizationProfileFamily::range;
    case ExecutionTypeFamily::document:
    case ExecutionTypeFamily::structured:
      return TypeCanonicalizationProfileFamily::document;
    case ExecutionTypeFamily::spatial:
      return TypeCanonicalizationProfileFamily::spatial;
    case ExecutionTypeFamily::vector:
      return TypeCanonicalizationProfileFamily::vector;
    case ExecutionTypeFamily::opaque:
      return TypeCanonicalizationProfileFamily::opaque;
    default:
      return TypeCanonicalizationProfileFamily::scalar;
  }
}

inline bool TypeIndexStatsOptimizerUuidVectorContains(
    const std::vector<Uuid>& values,
    const Uuid& needle) noexcept {
  for (const auto& value : values) {
    if (ExecutionDataPacketUuidEquals(value, needle)) {
      return true;
    }
  }
  return false;
}

inline bool TypeIndexStatsOptimizerFamilyVectorContains(
    const std::vector<TypeIndexFamily>& values,
    TypeIndexFamily needle) noexcept {
  for (const auto value : values) {
    if (value == needle) {
      return true;
    }
  }
  return false;
}

inline bool TypeIndexStatsOptimizerMetricPresent(
    const TypeIndexStatsOptimizerContract& contract,
    std::string_view metric_name) noexcept {
  for (const auto& candidate : contract.local_metric_names) {
    if (candidate == metric_name) {
      return true;
    }
  }
  return false;
}

inline bool TypeIndexStatsOptimizerCppUdrHookAdmitted(
    const DomainCppUdrOperationHook& hook) noexcept {
  return hook.present && !ExecutionDataPacketUuidIsNil(hook.library_uuid) &&
         !ExecutionDataPacketUuidIsNil(hook.mapping_descriptor_uuid) &&
         hook.mapping_descriptor_epoch != 0 &&
         !hook.entrypoint_symbol.empty() && hook.abi_major != 0 &&
         hook.preserves_descriptors && hook.parser_independent;
}

inline TypeIndexStatsOptimizerValidationResult
ValidateCanonicalComparisonContract(
    const CanonicalComparisonContract& contract,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(contract.comparison_contract_uuid)) {
    return {TypeIndexStatsOptimizerStatus::comparison_contract_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(contract.comparison_contract_uuid,
                                    decision.comparison_contract_uuid) ||
      !ExecutionDataPacketUuidEquals(contract.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::comparison_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.descriptor_epoch != decision.descriptor.descriptor_epoch) {
    return {TypeIndexStatsOptimizerStatus::comparison_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(contract.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {TypeIndexStatsOptimizerStatus::comparison_contract_invalid,
            descriptor_result.status, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(contract.descriptor.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::comparison_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!contract.descriptor_authoritative || !contract.parser_independent ||
      !ExecutionComparisonNullOrderingIsValid(contract.null_order_policy) ||
      !DomainMissingPolicyIsValid(contract.missing_policy)) {
    return {TypeIndexStatsOptimizerStatus::comparison_contract_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.resource_epoch != decision.resource_epoch) {
    return {TypeIndexStatsOptimizerStatus::
                comparison_resource_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.supports_equality &&
      ExecutionDataPacketUuidIsNil(contract.equality_operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                comparison_equality_operation_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.supports_ordering &&
      ExecutionDataPacketUuidIsNil(contract.ordering_operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                comparison_ordering_operation_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.supports_hashing &&
      ExecutionDataPacketUuidIsNil(contract.hash_operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                comparison_hash_operation_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.supports_hashing && !contract.hash_consistent_with_equality) {
    return {TypeIndexStatsOptimizerStatus::comparison_hash_equality_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if ((contract.supports_grouping || contract.supports_distinct ||
       contract.supports_join) &&
      !contract.grouping_consistent_with_equality) {
    return {TypeIndexStatsOptimizerStatus::
                comparison_grouping_equality_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (contract.supports_index &&
      (!contract.ordering_consistent_with_ordered_index ||
       contract.index_equivalence_class.empty())) {
    return {TypeIndexStatsOptimizerStatus::comparison_index_order_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.requires_canonicalization &&
      !ExecutionDataPacketUuidEquals(contract.canonicalization_profile_uuid,
                                    decision.canonicalization_profile_uuid)) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.reference_compatibility_required &&
      (ExecutionDataPacketUuidIsNil(contract.reference_profile_uuid) ||
       !contract.reference_policy_declared)) {
    return {TypeIndexStatsOptimizerStatus::reference_semantics_unsupported,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult ValidateCanonicalizationProfile(
    const CanonicalizationProfile& profile,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(profile.canonicalization_profile_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                canonicalization_profile_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(profile.canonicalization_profile_uuid,
                                    decision.canonicalization_profile_uuid) ||
      !ExecutionDataPacketUuidEquals(profile.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (profile.descriptor_epoch != decision.descriptor.descriptor_epoch) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(profile.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_profile_invalid,
            descriptor_result.status, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!profile.descriptor_authoritative || !profile.parser_independent ||
      !TypeCanonicalizationProfileFamilyIsValid(profile.profile_family) ||
      profile.profile_version == 0) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_profile_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (profile.resource_epoch != decision.resource_epoch ||
      ExecutionDataPacketUuidIsNil(profile.resource_profile_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                canonicalization_resource_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (profile.profile_family !=
      TypeIndexStatsOptimizerExpectedCanonicalizationFamily(
          decision.descriptor.family)) {
    return {TypeIndexStatsOptimizerStatus::canonicalization_family_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!profile.storage_normalization_declared ||
      !profile.comparison_normalization_declared ||
      !profile.hash_normalization_declared ||
      !profile.index_normalization_declared) {
    return {TypeIndexStatsOptimizerStatus::
                canonicalization_family_rules_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }

  bool family_rules_declared = true;
  switch (profile.profile_family) {
    case TypeCanonicalizationProfileFamily::text:
      family_rules_declared =
          profile.text_unicode_normalization_declared &&
          profile.text_case_accent_padding_declared &&
          profile.text_collation_key_declared &&
          profile.text_charset_validation_declared;
      break;
    case TypeCanonicalizationProfileFamily::numeric:
      family_rules_declared =
          profile.numeric_scale_decimal_context_declared &&
          profile.numeric_special_value_posture_declared &&
          profile.numeric_overflow_declared;
      break;
    case TypeCanonicalizationProfileFamily::temporal:
      family_rules_declared =
          profile.temporal_instant_display_declared &&
          profile.temporal_timezone_precision_declared &&
          profile.temporal_calendar_edge_declared;
      break;
    case TypeCanonicalizationProfileFamily::range:
      family_rules_declared =
          profile.range_bound_canonicalization_declared &&
          profile.range_merge_adjacency_declared;
      break;
    case TypeCanonicalizationProfileFamily::document:
      family_rules_declared =
          profile.document_key_order_declared &&
          profile.document_missing_null_declared;
      break;
    case TypeCanonicalizationProfileFamily::spatial:
      family_rules_declared =
          profile.spatial_reference_declared &&
          profile.spatial_validity_declared;
      break;
    case TypeCanonicalizationProfileFamily::vector:
      family_rules_declared =
          profile.vector_shape_declared && profile.vector_metric_declared;
      break;
    case TypeCanonicalizationProfileFamily::enum_set:
      family_rules_declared =
          profile.enum_label_policy_declared &&
          profile.enum_unknown_label_declared;
      break;
    case TypeCanonicalizationProfileFamily::opaque:
      family_rules_declared =
          profile.opaque_explicit_refusal ||
          (profile.opaque_cpp_udr_canonicalizer_present &&
           TypeIndexStatsOptimizerCppUdrHookAdmitted(
               profile.opaque_cpp_udr_hook));
      break;
    case TypeCanonicalizationProfileFamily::scalar:
      family_rules_declared = true;
      break;
  }
  if (!family_rules_declared) {
    return {TypeIndexStatsOptimizerStatus::
                canonicalization_family_rules_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult
ValidateIndexCompatibilityDescriptor(
    const IndexCompatibilityDescriptor& descriptor,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.index_compatibility_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                index_compatibility_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.index_compatibility_uuid,
                                    decision.index_compatibility_uuid) ||
      !ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::index_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.descriptor_epoch != decision.descriptor.descriptor_epoch) {
    return {TypeIndexStatsOptimizerStatus::index_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.comparison_contract_uuid,
                                    decision.comparison_contract_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                index_comparison_contract_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.descriptor_authoritative || !descriptor.parser_independent) {
    return {TypeIndexStatsOptimizerStatus::index_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!TypeIndexFamilyIsValid(descriptor.index_family) ||
      !TypeIndexStatsOptimizerFamilyVectorContains(
          decision.comparison_contract.index_equivalence_class,
          descriptor.index_family)) {
    return {TypeIndexStatsOptimizerStatus::index_family_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.key_encoding_profile_uuid)) {
    return {TypeIndexStatsOptimizerStatus::index_key_encoding_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.predicate_operation_refs.empty() ||
      !TypeIndexStatsOptimizerUuidVectorContains(
          descriptor.predicate_operation_refs, decision.operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::index_predicate_operation_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.covering_value_policy_declared ||
      !descriptor.partial_index_policy_declared ||
      !descriptor.expression_index_policy_declared) {
    return {TypeIndexStatsOptimizerStatus::index_policy_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.resource_dependencies_declared) {
    return {TypeIndexStatsOptimizerStatus::index_resource_dependency_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.rebuild_required_on_change ||
      !descriptor.descriptor_change_rebuild_required ||
      !descriptor.resource_change_rebuild_required) {
    return {TypeIndexStatsOptimizerStatus::index_rebuild_rule_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!TypeIndexLossinessPolicyIsValid(descriptor.lossiness_policy)) {
    return {TypeIndexStatsOptimizerStatus::index_lossiness_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.lossiness_policy ==
          TypeIndexLossinessPolicy::lossy_requires_recheck &&
      !descriptor.exact_recheck_required) {
    return {TypeIndexStatsOptimizerStatus::index_recheck_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.reference_compatibility_required &&
      (ExecutionDataPacketUuidIsNil(descriptor.reference_profile_uuid) ||
       !descriptor.reference_compatibility_declared)) {
    return {TypeIndexStatsOptimizerStatus::reference_semantics_unsupported,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult ValidateTypeStatisticsDescriptor(
    const TypeStatisticsDescriptor& descriptor,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.statistics_uuid)) {
    return {TypeIndexStatsOptimizerStatus::statistics_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.statistics_uuid,
                                    decision.statistics_uuid) ||
      !ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::statistics_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.descriptor_epoch != decision.descriptor.descriptor_epoch ||
      descriptor.schema_epoch != decision.schema_epoch ||
      descriptor.sample_epoch == 0 || descriptor.security_epoch == 0 ||
      descriptor.resource_epoch == 0) {
    return {TypeIndexStatsOptimizerStatus::statistics_epoch_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.resource_epoch != decision.resource_epoch) {
    return {TypeIndexStatsOptimizerStatus::selectivity_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.descriptor_authoritative || !descriptor.parser_independent) {
    return {TypeIndexStatsOptimizerStatus::statistics_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.source_object_uuid)) {
    return {TypeIndexStatsOptimizerStatus::statistics_source_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!TypeStatisticsSampleScopeIsValid(descriptor.sample_scope)) {
    return {TypeIndexStatsOptimizerStatus::statistics_scope_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.null_count > descriptor.row_count ||
      descriptor.missing_count > descriptor.row_count ||
      descriptor.distinct_count > descriptor.row_count ||
      !descriptor.null_count_declared || !descriptor.missing_count_declared ||
      !descriptor.distinct_count_declared) {
    return {TypeIndexStatsOptimizerStatus::statistics_count_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.privacy_policy_uuid) ||
      !descriptor.privacy_policy_enforced) {
    return {TypeIndexStatsOptimizerStatus::statistics_privacy_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.uses_protected_statistics &&
      !descriptor.protected_rare_values_redacted) {
    return {TypeIndexStatsOptimizerStatus::statistics_privacy_denied,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!descriptor.histogram_present ||
      ExecutionDataPacketUuidIsNil(descriptor.histogram.histogram_uuid) ||
      descriptor.histogram.histogram_kind.empty() ||
      descriptor.histogram.bucket_count == 0 ||
      !descriptor.histogram.descriptor_specific ||
      !descriptor.histogram.resource_epoch_bound ||
      !descriptor.histogram.protected_values_redacted) {
    return {TypeIndexStatsOptimizerStatus::statistics_histogram_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (descriptor.most_common_values.empty()) {
    return {TypeIndexStatsOptimizerStatus::statistics_mcv_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  for (const auto& mcv : descriptor.most_common_values) {
    if (ExecutionDataPacketUuidIsNil(mcv.value_summary_uuid) ||
        mcv.frequency == 0 || !mcv.descriptor_safe_summary ||
        !mcv.rare_value_redacted) {
      return {TypeIndexStatsOptimizerStatus::statistics_mcv_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
  }
  if (descriptor.element_path_stats_required &&
      descriptor.element_path_stats.empty()) {
    return {TypeIndexStatsOptimizerStatus::statistics_element_path_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  for (const auto& path : descriptor.element_path_stats) {
    const auto path_result = ValidateDomainElementPath(path);
    if (!path_result.ok()) {
      return {TypeIndexStatsOptimizerStatus::statistics_element_path_invalid,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              path_result.status, decision_index, 0};
    }
  }
  if (!ExecutionDataPacketUuidEquals(descriptor.selectivity_model_uuid,
                                    decision.selectivity_model_uuid)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_model_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult ValidateTypeSelectivityModel(
    const TypeSelectivityModel& model,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(model.selectivity_model_uuid)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_model_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(model.selectivity_model_uuid,
                                    decision.selectivity_model_uuid) ||
      !ExecutionDataPacketUuidEquals(model.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(model.operation_uuid,
                                    decision.operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_operation_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(model.statistics_uuid,
                                    decision.statistics_uuid)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_statistics_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (model.schema_epoch != decision.schema_epoch ||
      model.security_epoch != decision.security_epoch ||
      model.resource_epoch != decision.resource_epoch) {
    return {TypeIndexStatsOptimizerStatus::selectivity_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!TypeSelectivityUnknownPolicyIsValid(model.unknown_policy)) {
    return {TypeIndexStatsOptimizerStatus::selectivity_policy_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!model.unknown_selectivity_explicit) {
    return {TypeIndexStatsOptimizerStatus::selectivity_unknown_not_explicit,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!model.descriptor_compatible || !model.rejects_descriptor_mismatch ||
      !model.rejects_resource_mismatch || !model.rejects_security_mismatch ||
      !model.protected_value_leak_prevented) {
    return {TypeIndexStatsOptimizerStatus::
                selectivity_incompatible_stats_not_rejected,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (model.non_cpp_udr_metadata_requested) {
    return {TypeIndexStatsOptimizerStatus::non_cpp_udr_metadata_forbidden,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!model.cpp_udr_metadata_trusted ||
      (!ExecutionDataPacketUuidIsNil(
           model.cpp_udr_metadata_hook.library_uuid) &&
       !TypeIndexStatsOptimizerCppUdrHookAdmitted(
           model.cpp_udr_metadata_hook))) {
    return {TypeIndexStatsOptimizerStatus::udr_metadata_untrusted,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult
ValidateTypeOptimizerCacheKey(
    const TypeOptimizerCacheKeyDescriptor& cache_key,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(cache_key.database_uuid) ||
      ExecutionDataPacketUuidIsNil(cache_key.query_scope_uuid) ||
      ExecutionDataPacketUuidIsNil(cache_key.descriptor_uuid) ||
      ExecutionDataPacketUuidIsNil(cache_key.operation_uuid) ||
      cache_key.plan_cache_key_hash.empty()) {
    return {TypeIndexStatsOptimizerStatus::cache_key_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (cache_key.schema_epoch != decision.schema_epoch ||
      cache_key.security_epoch != decision.security_epoch ||
      cache_key.resource_epoch != decision.resource_epoch) {
    return {TypeIndexStatsOptimizerStatus::cache_key_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!cache_key.includes_descriptor || !cache_key.includes_operation ||
      !cache_key.includes_schema_epoch || !cache_key.includes_security_epoch ||
      !cache_key.includes_resource_epoch) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidIsNil(decision.domain_uuid) &&
      (!cache_key.includes_domain_when_present ||
       cache_key.domain_stack_hash.empty())) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.requires_comparison &&
      !cache_key.includes_comparison_when_present) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.requires_canonicalization &&
      !cache_key.includes_canonicalization_when_present) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.considers_index &&
      (!cache_key.includes_index_when_present ||
       ExecutionDataPacketUuidIsNil(cache_key.index_uuid) ||
       cache_key.index_generation == 0)) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.considers_statistics &&
      (!cache_key.includes_statistics_when_present ||
       !ExecutionDataPacketUuidEquals(cache_key.statistics_uuid,
                                      decision.statistics_uuid) ||
       cache_key.sample_epoch != decision.statistics.sample_epoch)) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.reference_compatibility_required &&
      !cache_key.includes_reference_when_present) {
    return {TypeIndexStatsOptimizerStatus::cache_key_dependency_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!cache_key.parser_family_untrusted_context_only) {
    return {TypeIndexStatsOptimizerStatus::cache_key_parser_authority,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult
ValidateTypeOptimizerDiagnostic(
    const TypeOptimizerDiagnosticVector& diagnostic,
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (diagnostic.diagnostic_code.empty()) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_code_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(diagnostic.descriptor_uuid,
                                    decision.descriptor_uuid) ||
      !ExecutionDataPacketUuidEquals(diagnostic.operation_uuid,
                                    decision.operation_uuid) ||
      diagnostic.resource_epoch != decision.resource_epoch ||
      diagnostic.security_epoch != decision.security_epoch) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_context_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.requires_comparison &&
      ExecutionDataPacketUuidIsNil(diagnostic.comparison_contract_uuid)) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_context_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.considers_index &&
      ExecutionDataPacketUuidIsNil(diagnostic.index_compatibility_uuid)) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_context_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.considers_statistics &&
      ExecutionDataPacketUuidIsNil(diagnostic.statistics_uuid)) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_context_missing,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!diagnostic.redaction_state_declared) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_redaction_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult ValidateTypeOptimizerDecision(
    const TypeOptimizerDecisionInput& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(decision.decision_uuid)) {
    return {TypeIndexStatsOptimizerStatus::decision_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(decision.query_scope_uuid)) {
    return {TypeIndexStatsOptimizerStatus::query_scope_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (ExecutionDataPacketUuidIsNil(decision.operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::operation_uuid_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.schema_epoch == 0) {
    return {TypeIndexStatsOptimizerStatus::schema_epoch_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.security_epoch == 0) {
    return {TypeIndexStatsOptimizerStatus::security_epoch_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.resource_epoch == 0) {
    return {TypeIndexStatsOptimizerStatus::resource_epoch_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!TypeOptimizerAdmissionStateIsValid(decision.admission_result)) {
    return {TypeIndexStatsOptimizerStatus::admission_state_invalid,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }

  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(decision.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {TypeIndexStatsOptimizerStatus::descriptor_invalid,
            descriptor_result.status, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(decision.descriptor.descriptor_uuid,
                                    decision.descriptor_uuid)) {
    return {TypeIndexStatsOptimizerStatus::descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.descriptor.descriptor_epoch != decision.schema_epoch) {
    return {TypeIndexStatsOptimizerStatus::descriptor_epoch_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!decision.descriptor_visible) {
    return {TypeIndexStatsOptimizerStatus::descriptor_not_visible,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!decision.operation_visible) {
    return {TypeIndexStatsOptimizerStatus::operation_not_visible,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!decision.operation_executable) {
    return {TypeIndexStatsOptimizerStatus::operation_not_executable,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!decision.operation_entry_present) {
    return {TypeIndexStatsOptimizerStatus::operation_descriptor_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  const auto operation_result =
      ValidateTypeOperationRegistryEntry(decision.operation_entry,
                                         decision_index);
  if (!operation_result.ok()) {
    return {TypeIndexStatsOptimizerStatus::operation_descriptor_invalid,
            operation_result.descriptor_status, operation_result.status,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!ExecutionDataPacketUuidEquals(decision.operation_entry.operation_uuid,
                                    decision.operation_uuid)) {
    return {TypeIndexStatsOptimizerStatus::
                operation_descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }

  const bool family_requires_profile =
      TypeIndexStatsOptimizerDescriptorFamilyRequiresProfile(
          decision.descriptor.family);
  if (decision.requires_comparison) {
    if (!decision.comparison_contract_present) {
      return {TypeIndexStatsOptimizerStatus::comparison_contract_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
    const auto comparison_result =
        ValidateCanonicalComparisonContract(decision.comparison_contract,
                                            decision, decision_index);
    if (!comparison_result.ok()) {
      return comparison_result;
    }
  }
  if (decision.requires_canonicalization || family_requires_profile) {
    if (!decision.canonicalization_profile_present) {
      return {TypeIndexStatsOptimizerStatus::
                  canonicalization_profile_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
    const auto canonicalization_result =
        ValidateCanonicalizationProfile(decision.canonicalization_profile,
                                        decision, decision_index);
    if (!canonicalization_result.ok()) {
      return canonicalization_result;
    }
  }
  if (decision.considers_index) {
    if (!decision.index_compatibility_present) {
      return {TypeIndexStatsOptimizerStatus::index_compatibility_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
    const auto index_result =
        ValidateIndexCompatibilityDescriptor(decision.index_compatibility,
                                             decision, decision_index);
    if (!index_result.ok()) {
      return index_result;
    }
    if (decision.selected_lossy_index &&
        !decision.exact_recheck_obligation_present) {
      return {TypeIndexStatsOptimizerStatus::index_recheck_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
  }
  if (decision.considers_statistics) {
    if (!decision.statistics_present) {
      return {TypeIndexStatsOptimizerStatus::statistics_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
    const auto statistics_result =
        ValidateTypeStatisticsDescriptor(decision.statistics, decision,
                                         decision_index);
    if (!statistics_result.ok()) {
      return statistics_result;
    }
    if (!decision.selectivity_model_present) {
      return {TypeIndexStatsOptimizerStatus::selectivity_model_required,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, decision_index, 0};
    }
    const auto selectivity_result =
        ValidateTypeSelectivityModel(decision.selectivity_model, decision,
                                     decision_index);
    if (!selectivity_result.ok()) {
      return selectivity_result;
    }
  }
  if (!decision.security_policy_permits) {
    return {TypeIndexStatsOptimizerStatus::security_policy_refused,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.reference_compatibility_required &&
      !decision.reference_semantics_preserved) {
    return {TypeIndexStatsOptimizerStatus::reference_semantics_unsupported,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.udr_metadata_requested && !decision.udr_metadata_cpp) {
    return {TypeIndexStatsOptimizerStatus::non_cpp_udr_metadata_forbidden,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.udr_metadata_requested && !decision.udr_metadata_trusted) {
    return {TypeIndexStatsOptimizerStatus::udr_metadata_untrusted,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (!decision.cache_key_present) {
    return {TypeIndexStatsOptimizerStatus::cache_key_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  const auto cache_key_result =
      ValidateTypeOptimizerCacheKey(decision.cache_key, decision,
                                    decision_index);
  if (!cache_key_result.ok()) {
    return cache_key_result;
  }
  if (!decision.diagnostic_present) {
    return {TypeIndexStatsOptimizerStatus::diagnostic_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  const auto diagnostic_result =
      ValidateTypeOptimizerDiagnostic(decision.diagnostic, decision,
                                      decision_index);
  if (!diagnostic_result.ok()) {
    return diagnostic_result;
  }
  const bool safe_result =
      decision.admission_result == TypeOptimizerAdmissionState::admitted ||
      decision.admission_result ==
          TypeOptimizerAdmissionState::admitted_conservative ||
      decision.admission_result == TypeOptimizerAdmissionState::requires_recheck;
  if (!safe_result) {
    return {TypeIndexStatsOptimizerStatus::unsafe_admission_result,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.admission_result ==
          TypeOptimizerAdmissionState::admitted_conservative &&
      !decision.conservative_estimation_allowed) {
    return {TypeIndexStatsOptimizerStatus::unsafe_admission_result,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  if (decision.admission_result == TypeOptimizerAdmissionState::requires_recheck &&
      !decision.exact_recheck_obligation_present) {
    return {TypeIndexStatsOptimizerStatus::index_recheck_required,
            ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
            DomainElementPathStatus::ok, decision_index, 0};
  }
  return {};
}

constexpr bool TypeIndexStatsOptimizerFamilyRequiresMatrixRow(
    ExecutionTypeFamily family) noexcept {
  switch (family) {
    case ExecutionTypeFamily::boolean:
    case ExecutionTypeFamily::signed_integer:
    case ExecutionTypeFamily::unsigned_integer:
    case ExecutionTypeFamily::real:
    case ExecutionTypeFamily::decimal:
    case ExecutionTypeFamily::uuid:
    case ExecutionTypeFamily::character:
    case ExecutionTypeFamily::binary:
    case ExecutionTypeFamily::bit_string:
    case ExecutionTypeFamily::temporal:
    case ExecutionTypeFamily::blob:
    case ExecutionTypeFamily::network:
    case ExecutionTypeFamily::document:
    case ExecutionTypeFamily::search:
    case ExecutionTypeFamily::structured:
    case ExecutionTypeFamily::range:
    case ExecutionTypeFamily::spatial:
    case ExecutionTypeFamily::vector:
    case ExecutionTypeFamily::graph:
    case ExecutionTypeFamily::time_series:
    case ExecutionTypeFamily::columnar:
    case ExecutionTypeFamily::aggregate_state:
    case ExecutionTypeFamily::sketch:
    case ExecutionTypeFamily::locator:
    case ExecutionTypeFamily::opaque:
      return true;
    case ExecutionTypeFamily::null_type:
    case ExecutionTypeFamily::result_set:
    case ExecutionTypeFamily::unknown:
      return false;
  }
  return false;
}

inline bool TypeIndexStatsOptimizerMatrixHasFamily(
    const TypeIndexStatsOptimizerContract& contract,
    ExecutionTypeFamily family) noexcept {
  for (const auto& row : contract.index_family_matrix) {
    if (row.type_family == family) {
      return true;
    }
  }
  return false;
}

inline TypeIndexStatsOptimizerValidationResult
ValidateTypeIndexFamilyMatrix(
    const TypeIndexStatsOptimizerContract& contract) {
  if (contract.index_family_matrix.empty()) {
    return {TypeIndexStatsOptimizerStatus::index_family_matrix_required};
  }
  constexpr ExecutionTypeFamily required_families[] = {
      ExecutionTypeFamily::boolean,
      ExecutionTypeFamily::signed_integer,
      ExecutionTypeFamily::unsigned_integer,
      ExecutionTypeFamily::real,
      ExecutionTypeFamily::decimal,
      ExecutionTypeFamily::uuid,
      ExecutionTypeFamily::character,
      ExecutionTypeFamily::binary,
      ExecutionTypeFamily::bit_string,
      ExecutionTypeFamily::temporal,
      ExecutionTypeFamily::blob,
      ExecutionTypeFamily::network,
      ExecutionTypeFamily::document,
      ExecutionTypeFamily::search,
      ExecutionTypeFamily::structured,
      ExecutionTypeFamily::range,
      ExecutionTypeFamily::spatial,
      ExecutionTypeFamily::vector,
      ExecutionTypeFamily::graph,
      ExecutionTypeFamily::time_series,
      ExecutionTypeFamily::columnar,
      ExecutionTypeFamily::aggregate_state,
      ExecutionTypeFamily::sketch,
      ExecutionTypeFamily::locator,
      ExecutionTypeFamily::opaque};
  for (const auto family : required_families) {
    if (!TypeIndexStatsOptimizerMatrixHasFamily(contract, family)) {
      return {
          TypeIndexStatsOptimizerStatus::index_family_matrix_family_missing};
    }
  }

  for (std::size_t row_index = 0;
       row_index < contract.index_family_matrix.size(); ++row_index) {
    const auto& row = contract.index_family_matrix[row_index];
    if (!TypeIndexStatsOptimizerFamilyRequiresMatrixRow(row.type_family)) {
      return {TypeIndexStatsOptimizerStatus::
                  index_family_matrix_family_missing,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, 0, row_index};
    }
    for (std::size_t previous = 0; previous < row_index; ++previous) {
      if (contract.index_family_matrix[previous].type_family ==
          row.type_family) {
        return {TypeIndexStatsOptimizerStatus::
                    index_family_matrix_family_duplicate,
                ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
                DomainElementPathStatus::ok, 0, row_index};
      }
    }
    if (row.classifications.empty()) {
      return {TypeIndexStatsOptimizerStatus::
                  index_family_matrix_classification_missing,
              ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
              DomainElementPathStatus::ok, 0, row_index};
    }
    constexpr TypeIndexFamily required_index_families[] = {
        TypeIndexFamily::btree,     TypeIndexFamily::hash,
        TypeIndexFamily::bitmap,    TypeIndexFamily::inverted,
        TypeIndexFamily::full_text, TypeIndexFamily::spatial,
        TypeIndexFamily::vector,    TypeIndexFamily::columnar,
        TypeIndexFamily::bloom,     TypeIndexFamily::sketch,
        TypeIndexFamily::graph,     TypeIndexFamily::time_series,
        TypeIndexFamily::extension};
    for (const auto family : required_index_families) {
      bool found = false;
      for (const auto& classification : row.classifications) {
        if (classification.index_family == family) {
          found = true;
          break;
        }
      }
      if (!found) {
        return {TypeIndexStatsOptimizerStatus::
                    index_family_matrix_classification_missing,
                ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
                DomainElementPathStatus::ok, 0, row_index};
      }
    }
    for (std::size_t classification_index = 0;
         classification_index < row.classifications.size();
         ++classification_index) {
      const auto& classification = row.classifications[classification_index];
      if (!TypeIndexFamilyIsValid(classification.index_family) ||
          !TypeIndexFamilySupportIsValid(classification.support)) {
        return {TypeIndexStatsOptimizerStatus::
                    index_family_matrix_support_invalid,
                ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
                DomainElementPathStatus::ok, 0, row_index};
      }
      for (std::size_t previous = 0; previous < classification_index;
           ++previous) {
        if (row.classifications[previous].index_family ==
            classification.index_family) {
          return {TypeIndexStatsOptimizerStatus::
                      index_family_matrix_index_duplicate,
                  ExecutionDataPacketStatus::ok,
                  TypeOperationRegistryStatus::ok, DomainElementPathStatus::ok,
                  0, row_index};
        }
      }
      if ((classification.support == TypeIndexFamilySupport::unsupported ||
           classification.support == TypeIndexFamilySupport::deferred) &&
          !classification.diagnostic_declared) {
        return {TypeIndexStatsOptimizerStatus::
                    index_family_matrix_diagnostic_required,
                ExecutionDataPacketStatus::ok, TypeOperationRegistryStatus::ok,
                DomainElementPathStatus::ok, 0, row_index};
      }
    }
  }
  return {};
}

inline TypeIndexStatsOptimizerValidationResult
ValidateTypeIndexStatsOptimizerContract(
    const TypeIndexStatsOptimizerContract& contract) {
  if (ExecutionDataPacketUuidIsNil(contract.contract_uuid)) {
    return {TypeIndexStatsOptimizerStatus::contract_uuid_required};
  }
  if (contract.contract_epoch == 0) {
    return {TypeIndexStatsOptimizerStatus::contract_epoch_required};
  }
  if (contract.stable_name.empty()) {
    return {TypeIndexStatsOptimizerStatus::stable_name_required};
  }
  if (!contract.descriptor_authoritative) {
    return {TypeIndexStatsOptimizerStatus::descriptor_not_authoritative};
  }
  if (!contract.parser_independent) {
    return {TypeIndexStatsOptimizerStatus::descriptor_parser_dependent};
  }
  if (contract.decisions.empty()) {
    return {TypeIndexStatsOptimizerStatus::decisions_required};
  }
  if (!contract.local_metrics_root_declared) {
    return {TypeIndexStatsOptimizerStatus::local_metrics_root_required};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.optimizer.type_index_stats.admission_count",
      "sys.metrics.optimizer.type_index_stats.refusal_count",
      "sys.metrics.optimizer.type_index_stats.conservative_admission_count",
      "sys.metrics.optimizer.type_index_stats.comparison_contract_missing_count",
      "sys.metrics.optimizer.type_index_stats.index_compatibility_missing_count",
      "sys.metrics.optimizer.type_index_stats.statistics_epoch_mismatch_count",
      "sys.metrics.optimizer.type_index_stats.statistics_privacy_denied_count",
      "sys.metrics.optimizer.type_index_stats.unknown_selectivity_count",
      "sys.metrics.optimizer.type_index_stats.plan_cache_invalidations",
      "sys.metrics.optimizer.type_index_stats.udr_metadata_refusal_count",
      "sys.metrics.optimizer.type_index_stats.reference_semantics_refusal_count",
      "sys.metrics.optimizer.type_index_stats.recheck_required_plan_count"};
  for (std::string_view metric : required_metrics) {
    if (!TypeIndexStatsOptimizerMetricPresent(contract, metric)) {
      return {TypeIndexStatsOptimizerStatus::local_metric_missing};
    }
  }
  if (!contract.cluster_metrics_guarded_by_cluster_governance) {
    return {TypeIndexStatsOptimizerStatus::cluster_metrics_guard_required};
  }

  const auto matrix_result = ValidateTypeIndexFamilyMatrix(contract);
  if (!matrix_result.ok()) {
    return matrix_result;
  }
  for (std::size_t decision_index = 0;
       decision_index < contract.decisions.size(); ++decision_index) {
    const auto decision_result =
        ValidateTypeOptimizerDecision(contract.decisions[decision_index],
                                      decision_index);
    if (!decision_result.ok()) {
      return decision_result;
    }
  }
  return {};
}

// SEARCH_KEY: TMD-TYPE-METADATA-DIAGNOSTICS-DRIVER
// SEARCH_KEY: TMD-CATALOG-EXPOSURE-MODEL
// SEARCH_KEY: TMD-DRIVER-TYPE-METADATA-DESCRIPTOR
// SEARCH_KEY: TMD-DIAGNOSTIC-COMPATIBILITY-MODEL
// SEARCH_KEY: TMD-BACKUP-RESTORE-TYPE-PROFILE
// SEARCH_KEY: TMD-REPLICATION-TYPE-PROFILE
// SEARCH_KEY: TMD-CLUSTER-TYPE-TRANSPORT-PROFILE
// SEARCH_KEY: TMD-UNSUPPORTED-DEGRADED-CONTRACT
// SEARCH_KEY: TMD-TYPE-TESTING-CORPUS
enum class TypeMetadataExposureClass : std::uint8_t {
  canonical = 0,
  driver_rendered = 1,
  reference_rendered = 2,
  support_redacted = 3,
  security_redacted = 4,
  render_only = 5,
  bridge_only = 6,
  deferred = 7,
  unsupported_by_policy = 8
};

constexpr bool TypeMetadataExposureClassIsValid(
    TypeMetadataExposureClass exposure) noexcept {
  switch (exposure) {
    case TypeMetadataExposureClass::canonical:
    case TypeMetadataExposureClass::driver_rendered:
    case TypeMetadataExposureClass::reference_rendered:
    case TypeMetadataExposureClass::support_redacted:
    case TypeMetadataExposureClass::security_redacted:
    case TypeMetadataExposureClass::render_only:
    case TypeMetadataExposureClass::bridge_only:
    case TypeMetadataExposureClass::deferred:
    case TypeMetadataExposureClass::unsupported_by_policy:
      return true;
  }
  return false;
}

enum class DriverMetadataFamily : std::uint8_t {
  odbc = 0,
  jdbc = 1,
  dotnet = 2,
  native = 3,
  reference_specific = 4
};

constexpr bool DriverMetadataFamilyIsValid(
    DriverMetadataFamily family) noexcept {
  switch (family) {
    case DriverMetadataFamily::odbc:
    case DriverMetadataFamily::jdbc:
    case DriverMetadataFamily::dotnet:
    case DriverMetadataFamily::native:
    case DriverMetadataFamily::reference_specific:
      return true;
  }
  return false;
}

enum class TypeMetadataCompatibilityClass : std::uint8_t {
  native_or_better = 0,
  degraded = 1,
  bridge_only = 2,
  render_only = 3,
  deferred = 4,
  unsupported_by_policy = 5
};

constexpr bool TypeMetadataCompatibilityClassIsValid(
    TypeMetadataCompatibilityClass compatibility) noexcept {
  switch (compatibility) {
    case TypeMetadataCompatibilityClass::native_or_better:
    case TypeMetadataCompatibilityClass::degraded:
    case TypeMetadataCompatibilityClass::bridge_only:
    case TypeMetadataCompatibilityClass::render_only:
    case TypeMetadataCompatibilityClass::deferred:
    case TypeMetadataCompatibilityClass::unsupported_by_policy:
      return true;
  }
  return false;
}

constexpr bool TypeMetadataCompatibilityClassExecutable(
    TypeMetadataCompatibilityClass compatibility) noexcept {
  return compatibility == TypeMetadataCompatibilityClass::native_or_better ||
         compatibility == TypeMetadataCompatibilityClass::degraded;
}

enum class DriverMetadataNullable : std::uint8_t {
  nullable = 0,
  not_nullable = 1,
  unknown_by_policy = 2
};

constexpr bool DriverMetadataNullableIsValid(
    DriverMetadataNullable value) noexcept {
  switch (value) {
    case DriverMetadataNullable::nullable:
    case DriverMetadataNullable::not_nullable:
    case DriverMetadataNullable::unknown_by_policy:
      return true;
  }
  return false;
}

enum class DriverMetadataSignedness : std::uint8_t {
  signed_numeric = 0,
  unsigned_numeric = 1,
  not_numeric = 2,
  unknown_by_policy = 3
};

constexpr bool DriverMetadataSignednessIsValid(
    DriverMetadataSignedness value) noexcept {
  switch (value) {
    case DriverMetadataSignedness::signed_numeric:
    case DriverMetadataSignedness::unsigned_numeric:
    case DriverMetadataSignedness::not_numeric:
    case DriverMetadataSignedness::unknown_by_policy:
      return true;
  }
  return false;
}

enum class DriverMetadataCaseSensitivity : std::uint8_t {
  case_sensitive = 0,
  case_insensitive = 1,
  collation_dependent = 2,
  unknown_by_policy = 3
};

constexpr bool DriverMetadataCaseSensitivityIsValid(
    DriverMetadataCaseSensitivity value) noexcept {
  switch (value) {
    case DriverMetadataCaseSensitivity::case_sensitive:
    case DriverMetadataCaseSensitivity::case_insensitive:
    case DriverMetadataCaseSensitivity::collation_dependent:
    case DriverMetadataCaseSensitivity::unknown_by_policy:
      return true;
  }
  return false;
}

enum class DriverMetadataSearchability : std::uint8_t {
  none = 0,
  exact = 1,
  range = 2,
  full_text = 3,
  spatial = 4,
  vector = 5,
  reference_defined = 6,
  unknown_by_policy = 7
};

constexpr bool DriverMetadataSearchabilityIsValid(
    DriverMetadataSearchability value) noexcept {
  switch (value) {
    case DriverMetadataSearchability::none:
    case DriverMetadataSearchability::exact:
    case DriverMetadataSearchability::range:
    case DriverMetadataSearchability::full_text:
    case DriverMetadataSearchability::spatial:
    case DriverMetadataSearchability::vector:
    case DriverMetadataSearchability::reference_defined:
    case DriverMetadataSearchability::unknown_by_policy:
      return true;
  }
  return false;
}

enum class TypeMetadataDisclosureClass : std::uint8_t {
  public_metadata = 0,
  privileged_metadata = 1,
  support_redacted = 2,
  security_redacted = 3,
  denied = 4
};

constexpr bool TypeMetadataDisclosureClassIsValid(
    TypeMetadataDisclosureClass value) noexcept {
  switch (value) {
    case TypeMetadataDisclosureClass::public_metadata:
    case TypeMetadataDisclosureClass::privileged_metadata:
    case TypeMetadataDisclosureClass::support_redacted:
    case TypeMetadataDisclosureClass::security_redacted:
    case TypeMetadataDisclosureClass::denied:
      return true;
  }
  return false;
}

enum class TypeMetadataRedactionState : std::uint8_t {
  none = 0,
  redacted = 1,
  hidden = 2,
  denied = 3,
  generalized = 4
};

constexpr bool TypeMetadataRedactionStateIsValid(
    TypeMetadataRedactionState value) noexcept {
  switch (value) {
    case TypeMetadataRedactionState::none:
    case TypeMetadataRedactionState::redacted:
    case TypeMetadataRedactionState::hidden:
    case TypeMetadataRedactionState::denied:
    case TypeMetadataRedactionState::generalized:
      return true;
  }
  return false;
}

enum class TypeMetadataDiagnosticPhase : std::uint8_t {
  reference_parse = 0,
  reference_translation = 1,
  scratchbird_execution = 2,
  cpp_udr_bridge = 3,
  reference_rendering = 4,
  metadata_exposure = 5,
  backup_restore = 6,
  replication = 7,
  cluster_transport = 8,
  policy_enforcement = 9
};

constexpr bool TypeMetadataDiagnosticPhaseIsValid(
    TypeMetadataDiagnosticPhase phase) noexcept {
  switch (phase) {
    case TypeMetadataDiagnosticPhase::reference_parse:
    case TypeMetadataDiagnosticPhase::reference_translation:
    case TypeMetadataDiagnosticPhase::scratchbird_execution:
    case TypeMetadataDiagnosticPhase::cpp_udr_bridge:
    case TypeMetadataDiagnosticPhase::reference_rendering:
    case TypeMetadataDiagnosticPhase::metadata_exposure:
    case TypeMetadataDiagnosticPhase::backup_restore:
    case TypeMetadataDiagnosticPhase::replication:
    case TypeMetadataDiagnosticPhase::cluster_transport:
    case TypeMetadataDiagnosticPhase::policy_enforcement:
      return true;
  }
  return false;
}

struct TypeMetadataCatalogExposureModel {
  Uuid exposure_model_uuid{};
  Uuid catalog_snapshot_uuid{};
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  bool type_descriptors_exposed = true;
  bool domain_descriptors_exposed = true;
  bool reference_mappings_exposed = true;
  bool capabilities_exposed = true;
  bool operations_exposed = true;
  bool index_statistics_exposed = true;
  bool driver_metadata_exposed = true;
  bool backup_replication_exposed = true;
  bool unsupported_deferred_exposed = true;
  bool uuid_authority_preserved = true;
  bool display_names_non_authoritative = true;
  bool security_policy_applied = true;
  bool reference_views_do_not_create_authority = true;
  bool status_and_version_exposed = true;
};

struct DriverTypeMetadataDescriptor {
  Uuid driver_metadata_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  std::uint64_t descriptor_epoch = 0;
  ExecutionTypeDescriptor descriptor;
  std::string reference_family;
  std::string reference_version_profile;
  DriverMetadataFamily driver_family = DriverMetadataFamily::native;
  std::string type_name;
  std::string native_type_code;
  std::string sql_type_code;
  std::int64_t precision = 0;
  std::int64_t scale = 0;
  std::int64_t display_size = 0;
  DriverMetadataNullable nullable = DriverMetadataNullable::nullable;
  DriverMetadataSignedness signedness =
      DriverMetadataSignedness::not_numeric;
  DriverMetadataCaseSensitivity case_sensitive =
      DriverMetadataCaseSensitivity::unknown_by_policy;
  DriverMetadataSearchability searchable = DriverMetadataSearchability::none;
  bool currency = false;
  bool auto_increment = false;
  std::string literal_prefix;
  std::string literal_suffix;
  std::vector<std::string> create_params;
  std::uint32_t array_rank = 0;
  Uuid element_descriptor_uuid{};
  TypeMetadataCompatibilityClass compatibility_class =
      TypeMetadataCompatibilityClass::native_or_better;
  std::string unsupported_reason;
  TypeMetadataExposureClass exposure_class =
      TypeMetadataExposureClass::driver_rendered;
  TypeMetadataDisclosureClass visibility_class =
      TypeMetadataDisclosureClass::public_metadata;
  TypeMetadataRedactionState redaction_state =
      TypeMetadataRedactionState::none;
  Uuid diagnostic_policy_ref{};
  std::string definition_hash;
  bool canonical_descriptor_authority_preserved = true;
  bool reference_name_not_authority = true;
  bool protocol_code_not_authority = true;
  bool derived_without_execution = true;
};

struct TypeMetadataDiagnosticPayload {
  Uuid diagnostic_uuid{};
  std::string scratchbird_code;
  std::string severity;
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  Uuid operation_uuid{};
  std::string reference_family;
  std::string reference_version_profile;
  DriverMetadataFamily driver_family = DriverMetadataFamily::native;
  TypeMetadataCompatibilityClass compatibility_class =
      TypeMetadataCompatibilityClass::native_or_better;
  TypeMetadataDiagnosticPhase source_phase =
      TypeMetadataDiagnosticPhase::metadata_exposure;
  TypeMetadataDisclosureClass disclosure_class =
      TypeMetadataDisclosureClass::public_metadata;
  TypeMetadataRedactionState redaction_state =
      TypeMetadataRedactionState::none;
  std::string stable_search_key;
  std::vector<std::string> reference_message_vector;
  bool reference_message_permitted = false;
  bool protected_values_redacted = true;
  bool hidden_metadata_redacted = true;
  bool bridge_targets_redacted = true;
  bool encryption_material_redacted = true;
  bool raw_paths_redacted = true;
  bool privileged_catalog_rows_redacted = true;
};

struct BackupRestoreTypeProfile {
  Uuid profile_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  Uuid canonical_encoding_uuid{};
  Uuid physical_encoding_uuid{};
  Uuid logical_rendering_uuid{};
  Uuid descriptor_snapshot_uuid{};
  std::uint64_t resource_epoch = 0;
  Uuid resource_profile_uuid{};
  bool logical_backup_supported = true;
  bool physical_backup_supported = true;
  bool restore_admission_supported = true;
  bool descriptor_snapshot_required = true;
  bool version_migration_declared = true;
  bool unsupported_values_refuse = true;
  bool protected_value_policy_declared = true;
  bool opaque_locator_policy_declared = true;
  bool cpp_udr_dependency_required = false;
  DomainCppUdrOperationHook cpp_udr_dependency;
  TypeMetadataDiagnosticPayload diagnostic;
  std::string conformance_key;
};

struct ReplicationTypeProfile {
  Uuid profile_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  Uuid delta_encoding_uuid{};
  Uuid full_value_encoding_uuid{};
  std::uint64_t resource_epoch = 0;
  Uuid resource_profile_uuid{};
  bool replication_safe = true;
  bool delta_encoding_declared = true;
  bool full_value_replacement_declared = true;
  bool element_delta_policy_declared = true;
  bool ordering_causal_metadata_declared = true;
  bool version_compatibility_declared = true;
  bool conflict_behavior_declared = true;
  bool fails_closed_when_unproven = true;
  TypeMetadataDiagnosticPayload diagnostic;
  std::string conformance_key;
};

struct ClusterTypeTransportProfile {
  Uuid profile_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  Uuid transport_encoding_uuid{};
  Uuid resource_profile_uuid{};
  std::uint64_t resource_epoch = 0;
  bool compression_policy_declared = true;
  bool encryption_policy_declared = true;
  bool protected_value_policy_declared = true;
  bool resource_negotiation_declared = true;
  bool fallback_refusal_declared = true;
  bool route_policy_declared = true;
  bool rejects_incompatible_descriptors = true;
  bool rejects_incompatible_resources = true;
  bool rejects_incompatible_udrs = true;
  bool rejects_incompatible_policies = true;
  bool cluster_authority_recorded = false;
  bool cluster_governance_present = false;
  TypeMetadataDiagnosticPayload diagnostic;
  std::string conformance_key;
};

struct UnsupportedDegradedTypeContract {
  Uuid contract_uuid{};
  Uuid descriptor_uuid{};
  TypeMetadataCompatibilityClass compatibility_class =
      TypeMetadataCompatibilityClass::native_or_better;
  std::string diagnostic_code;
  std::string metadata_status;
  std::string unsupported_reason;
  bool metadata_visible = true;
  bool diagnostic_visible = true;
  bool silent_fallback_impossible = true;
  bool non_executable_when_not_native = true;
  bool bridge_availability_checked = true;
  bool cpp_bridge_required_for_bridge_only = true;
  bool non_cpp_bridge_forbidden = true;
  DomainCppUdrOperationHook bridge_hook;
};

struct TypeTestingCorpusDescriptor {
  Uuid corpus_uuid{};
  Uuid descriptor_uuid{};
  Uuid reference_mapping_uuid{};
  std::string conformance_manifest_hash;
  bool implemented_reference_mapping = false;
  bool creation_metadata_case = true;
  bool literal_bind_case = true;
  bool cast_case = true;
  bool operation_case = true;
  bool index_case = true;
  bool statistics_case = true;
  bool backup_restore_case = true;
  bool replication_case = true;
  bool driver_metadata_case = true;
  bool diagnostics_case = true;
};

struct TypeMetadataCacheKeyDescriptor {
  Uuid database_uuid{};
  Uuid descriptor_uuid{};
  Uuid domain_uuid{};
  std::string domain_stack_hash;
  DriverMetadataFamily driver_family = DriverMetadataFamily::native;
  std::string reference_family;
  std::string reference_version_profile;
  std::uint64_t schema_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::string capability_profile_definition_hash;
  Uuid policy_uuid{};
  Uuid parser_family_uuid{};
  bool includes_database = true;
  bool includes_descriptor = true;
  bool includes_domain_when_present = true;
  bool includes_driver_family = true;
  bool includes_reference_when_present = true;
  bool includes_schema_epoch = true;
  bool includes_security_epoch = true;
  bool includes_resource_epoch = true;
  bool includes_definition_hashes = true;
  bool includes_policy_uuid = true;
  bool parser_family_untrusted_context_only = true;
  bool metadata_query_causes_execution = false;
  bool metadata_query_loads_udr = false;
  bool metadata_query_mutates_storage = false;
  bool metadata_query_executes_parser_sql = false;
};

struct TypeMetadataDiagnosticsDriverContract {
  Uuid contract_uuid{};
  std::uint64_t contract_epoch = 0;
  std::string stable_name;
  TypeMetadataCatalogExposureModel catalog_exposure;
  std::vector<DriverTypeMetadataDescriptor> driver_metadata_rows;
  std::vector<TypeMetadataDiagnosticPayload> diagnostic_payloads;
  std::vector<BackupRestoreTypeProfile> backup_restore_profiles;
  std::vector<ReplicationTypeProfile> replication_profiles;
  std::vector<ClusterTypeTransportProfile> cluster_transport_profiles;
  std::vector<UnsupportedDegradedTypeContract> unsupported_contracts;
  std::vector<TypeTestingCorpusDescriptor> testing_corpus;
  TypeMetadataCacheKeyDescriptor cache_key;
  std::vector<std::string> local_metric_names;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool local_metrics_root_declared = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
};

enum class TypeMetadataDiagnosticsStatus : std::uint16_t {
  ok = 0,
  contract_uuid_required = 1,
  contract_epoch_required = 2,
  stable_name_required = 3,
  descriptor_not_authoritative = 4,
  descriptor_parser_dependent = 5,
  catalog_exposure_uuid_required = 6,
  catalog_snapshot_uuid_required = 7,
  catalog_epoch_required = 8,
  catalog_family_missing = 9,
  catalog_false_authority_guard_missing = 10,
  driver_rows_required = 11,
  driver_metadata_uuid_required = 12,
  driver_descriptor_uuid_required = 13,
  driver_descriptor_invalid = 14,
  driver_descriptor_uuid_mismatch = 15,
  driver_descriptor_epoch_mismatch = 16,
  driver_family_invalid = 17,
  driver_family_coverage_missing = 18,
  driver_reference_family_required = 19,
  driver_type_name_required = 20,
  driver_numeric_metadata_invalid = 21,
  driver_policy_enum_invalid = 22,
  driver_container_element_required = 23,
  compatibility_class_invalid = 24,
  unsupported_reason_required = 25,
  diagnostic_policy_required = 26,
  driver_definition_hash_required = 27,
  driver_false_authority_guard_missing = 28,
  diagnostic_payload_required = 29,
  diagnostic_uuid_required = 30,
  diagnostic_code_required = 31,
  diagnostic_descriptor_uuid_required = 32,
  diagnostic_policy_enum_invalid = 33,
  diagnostic_search_key_required = 34,
  diagnostic_redaction_failed = 35,
  reference_message_not_permitted = 36,
  backup_profile_required = 37,
  backup_profile_uuid_required = 38,
  backup_profile_identity_missing = 39,
  backup_profile_policy_missing = 40,
  backup_profile_udr_invalid = 41,
  replication_profile_required = 42,
  replication_profile_uuid_required = 43,
  replication_profile_identity_missing = 44,
  replication_profile_policy_missing = 45,
  cluster_profile_required = 46,
  cluster_profile_uuid_required = 47,
  cluster_profile_identity_missing = 48,
  cluster_profile_policy_missing = 49,
  cluster_authority_without_governance = 50,
  unsupported_contract_required = 51,
  unsupported_contract_uuid_required = 52,
  unsupported_contract_diagnostic_required = 53,
  unsupported_contract_silent_fallback = 54,
  unsupported_contract_bridge_invalid = 55,
  testing_corpus_required = 56,
  testing_corpus_uuid_required = 57,
  testing_corpus_manifest_required = 58,
  testing_corpus_case_missing = 59,
  cache_key_required = 60,
  cache_key_dependency_missing = 61,
  cache_key_epoch_missing = 62,
  cache_key_parser_authority = 63,
  metadata_query_side_effect = 64,
  local_metrics_root_required = 65,
  local_metric_missing = 66,
  cluster_metrics_guard_required = 67
};

constexpr std::string_view TypeMetadataDiagnosticsStatusName(
    TypeMetadataDiagnosticsStatus status) noexcept {
  switch (status) {
    case TypeMetadataDiagnosticsStatus::ok:
      return "ok";
    case TypeMetadataDiagnosticsStatus::contract_uuid_required:
      return "contract_uuid_required";
    case TypeMetadataDiagnosticsStatus::contract_epoch_required:
      return "contract_epoch_required";
    case TypeMetadataDiagnosticsStatus::stable_name_required:
      return "stable_name_required";
    case TypeMetadataDiagnosticsStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case TypeMetadataDiagnosticsStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case TypeMetadataDiagnosticsStatus::catalog_exposure_uuid_required:
      return "catalog_exposure_uuid_required";
    case TypeMetadataDiagnosticsStatus::catalog_snapshot_uuid_required:
      return "catalog_snapshot_uuid_required";
    case TypeMetadataDiagnosticsStatus::catalog_epoch_required:
      return "catalog_epoch_required";
    case TypeMetadataDiagnosticsStatus::catalog_family_missing:
      return "catalog_family_missing";
    case TypeMetadataDiagnosticsStatus::catalog_false_authority_guard_missing:
      return "catalog_false_authority_guard_missing";
    case TypeMetadataDiagnosticsStatus::driver_rows_required:
      return "driver_rows_required";
    case TypeMetadataDiagnosticsStatus::driver_metadata_uuid_required:
      return "driver_metadata_uuid_required";
    case TypeMetadataDiagnosticsStatus::driver_descriptor_uuid_required:
      return "driver_descriptor_uuid_required";
    case TypeMetadataDiagnosticsStatus::driver_descriptor_invalid:
      return "driver_descriptor_invalid";
    case TypeMetadataDiagnosticsStatus::driver_descriptor_uuid_mismatch:
      return "driver_descriptor_uuid_mismatch";
    case TypeMetadataDiagnosticsStatus::driver_descriptor_epoch_mismatch:
      return "driver_descriptor_epoch_mismatch";
    case TypeMetadataDiagnosticsStatus::driver_family_invalid:
      return "driver_family_invalid";
    case TypeMetadataDiagnosticsStatus::driver_family_coverage_missing:
      return "driver_family_coverage_missing";
    case TypeMetadataDiagnosticsStatus::driver_reference_family_required:
      return "driver_reference_family_required";
    case TypeMetadataDiagnosticsStatus::driver_type_name_required:
      return "driver_type_name_required";
    case TypeMetadataDiagnosticsStatus::driver_numeric_metadata_invalid:
      return "driver_numeric_metadata_invalid";
    case TypeMetadataDiagnosticsStatus::driver_policy_enum_invalid:
      return "driver_policy_enum_invalid";
    case TypeMetadataDiagnosticsStatus::driver_container_element_required:
      return "driver_container_element_required";
    case TypeMetadataDiagnosticsStatus::compatibility_class_invalid:
      return "compatibility_class_invalid";
    case TypeMetadataDiagnosticsStatus::unsupported_reason_required:
      return "unsupported_reason_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_policy_required:
      return "diagnostic_policy_required";
    case TypeMetadataDiagnosticsStatus::driver_definition_hash_required:
      return "driver_definition_hash_required";
    case TypeMetadataDiagnosticsStatus::driver_false_authority_guard_missing:
      return "driver_false_authority_guard_missing";
    case TypeMetadataDiagnosticsStatus::diagnostic_payload_required:
      return "diagnostic_payload_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_uuid_required:
      return "diagnostic_uuid_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_code_required:
      return "diagnostic_code_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_descriptor_uuid_required:
      return "diagnostic_descriptor_uuid_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_policy_enum_invalid:
      return "diagnostic_policy_enum_invalid";
    case TypeMetadataDiagnosticsStatus::diagnostic_search_key_required:
      return "diagnostic_search_key_required";
    case TypeMetadataDiagnosticsStatus::diagnostic_redaction_failed:
      return "diagnostic_redaction_failed";
    case TypeMetadataDiagnosticsStatus::reference_message_not_permitted:
      return "reference_message_not_permitted";
    case TypeMetadataDiagnosticsStatus::backup_profile_required:
      return "backup_profile_required";
    case TypeMetadataDiagnosticsStatus::backup_profile_uuid_required:
      return "backup_profile_uuid_required";
    case TypeMetadataDiagnosticsStatus::backup_profile_identity_missing:
      return "backup_profile_identity_missing";
    case TypeMetadataDiagnosticsStatus::backup_profile_policy_missing:
      return "backup_profile_policy_missing";
    case TypeMetadataDiagnosticsStatus::backup_profile_udr_invalid:
      return "backup_profile_udr_invalid";
    case TypeMetadataDiagnosticsStatus::replication_profile_required:
      return "replication_profile_required";
    case TypeMetadataDiagnosticsStatus::replication_profile_uuid_required:
      return "replication_profile_uuid_required";
    case TypeMetadataDiagnosticsStatus::replication_profile_identity_missing:
      return "replication_profile_identity_missing";
    case TypeMetadataDiagnosticsStatus::replication_profile_policy_missing:
      return "replication_profile_policy_missing";
    case TypeMetadataDiagnosticsStatus::cluster_profile_required:
      return "cluster_profile_required";
    case TypeMetadataDiagnosticsStatus::cluster_profile_uuid_required:
      return "cluster_profile_uuid_required";
    case TypeMetadataDiagnosticsStatus::cluster_profile_identity_missing:
      return "cluster_profile_identity_missing";
    case TypeMetadataDiagnosticsStatus::cluster_profile_policy_missing:
      return "cluster_profile_policy_missing";
    case TypeMetadataDiagnosticsStatus::cluster_authority_without_governance:
      return "cluster_authority_without_governance";
    case TypeMetadataDiagnosticsStatus::unsupported_contract_required:
      return "unsupported_contract_required";
    case TypeMetadataDiagnosticsStatus::unsupported_contract_uuid_required:
      return "unsupported_contract_uuid_required";
    case TypeMetadataDiagnosticsStatus::unsupported_contract_diagnostic_required:
      return "unsupported_contract_diagnostic_required";
    case TypeMetadataDiagnosticsStatus::unsupported_contract_silent_fallback:
      return "unsupported_contract_silent_fallback";
    case TypeMetadataDiagnosticsStatus::unsupported_contract_bridge_invalid:
      return "unsupported_contract_bridge_invalid";
    case TypeMetadataDiagnosticsStatus::testing_corpus_required:
      return "testing_corpus_required";
    case TypeMetadataDiagnosticsStatus::testing_corpus_uuid_required:
      return "testing_corpus_uuid_required";
    case TypeMetadataDiagnosticsStatus::testing_corpus_manifest_required:
      return "testing_corpus_manifest_required";
    case TypeMetadataDiagnosticsStatus::testing_corpus_case_missing:
      return "testing_corpus_case_missing";
    case TypeMetadataDiagnosticsStatus::cache_key_required:
      return "cache_key_required";
    case TypeMetadataDiagnosticsStatus::cache_key_dependency_missing:
      return "cache_key_dependency_missing";
    case TypeMetadataDiagnosticsStatus::cache_key_epoch_missing:
      return "cache_key_epoch_missing";
    case TypeMetadataDiagnosticsStatus::cache_key_parser_authority:
      return "cache_key_parser_authority";
    case TypeMetadataDiagnosticsStatus::metadata_query_side_effect:
      return "metadata_query_side_effect";
    case TypeMetadataDiagnosticsStatus::local_metrics_root_required:
      return "local_metrics_root_required";
    case TypeMetadataDiagnosticsStatus::local_metric_missing:
      return "local_metric_missing";
    case TypeMetadataDiagnosticsStatus::cluster_metrics_guard_required:
      return "cluster_metrics_guard_required";
  }
  return "unknown_status";
}

struct TypeMetadataDiagnosticsValidationResult {
  TypeMetadataDiagnosticsStatus status = TypeMetadataDiagnosticsStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t row_index = 0;

  bool ok() const noexcept {
    return status == TypeMetadataDiagnosticsStatus::ok;
  }
};

inline bool TypeMetadataDiagnosticsMetricPresent(
    const TypeMetadataDiagnosticsDriverContract& contract,
    std::string_view metric_name) noexcept {
  for (const auto& candidate : contract.local_metric_names) {
    if (candidate == metric_name) {
      return true;
    }
  }
  return false;
}

inline bool TypeMetadataDiagnosticsCppUdrHookAdmitted(
    const DomainCppUdrOperationHook& hook) noexcept {
  return hook.present && !ExecutionDataPacketUuidIsNil(hook.library_uuid) &&
         !ExecutionDataPacketUuidIsNil(hook.mapping_descriptor_uuid) &&
         hook.mapping_descriptor_epoch != 0 &&
         !hook.entrypoint_symbol.empty() && hook.abi_major != 0 &&
         hook.preserves_descriptors && hook.parser_independent;
}

inline TypeMetadataDiagnosticsValidationResult
ValidateTypeMetadataCatalogExposure(
    const TypeMetadataCatalogExposureModel& exposure) {
  if (ExecutionDataPacketUuidIsNil(exposure.exposure_model_uuid)) {
    return {TypeMetadataDiagnosticsStatus::catalog_exposure_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(exposure.catalog_snapshot_uuid)) {
    return {TypeMetadataDiagnosticsStatus::catalog_snapshot_uuid_required};
  }
  if (exposure.schema_epoch == 0 || exposure.security_epoch == 0 ||
      exposure.resource_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::catalog_epoch_required};
  }
  if (!exposure.type_descriptors_exposed ||
      !exposure.domain_descriptors_exposed || !exposure.reference_mappings_exposed ||
      !exposure.capabilities_exposed || !exposure.operations_exposed ||
      !exposure.index_statistics_exposed || !exposure.driver_metadata_exposed ||
      !exposure.backup_replication_exposed ||
      !exposure.unsupported_deferred_exposed) {
    return {TypeMetadataDiagnosticsStatus::catalog_family_missing};
  }
  if (!exposure.uuid_authority_preserved ||
      !exposure.display_names_non_authoritative ||
      !exposure.security_policy_applied ||
      !exposure.reference_views_do_not_create_authority ||
      !exposure.status_and_version_exposed) {
    return {TypeMetadataDiagnosticsStatus::
                catalog_false_authority_guard_missing};
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult
ValidateDriverTypeMetadataDescriptor(
    const DriverTypeMetadataDescriptor& row,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(row.driver_metadata_uuid)) {
    return {TypeMetadataDiagnosticsStatus::driver_metadata_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(row.descriptor_uuid)) {
    return {TypeMetadataDiagnosticsStatus::driver_descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(row.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {TypeMetadataDiagnosticsStatus::driver_descriptor_invalid,
            descriptor_result.status, row_index};
  }
  if (!ExecutionDataPacketUuidEquals(row.descriptor.descriptor_uuid,
                                    row.descriptor_uuid)) {
    return {TypeMetadataDiagnosticsStatus::driver_descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (row.descriptor.descriptor_epoch != row.descriptor_epoch) {
    return {TypeMetadataDiagnosticsStatus::driver_descriptor_epoch_mismatch,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!DriverMetadataFamilyIsValid(row.driver_family)) {
    return {TypeMetadataDiagnosticsStatus::driver_family_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (row.driver_family == DriverMetadataFamily::reference_specific &&
      (row.reference_family.empty() || row.reference_version_profile.empty())) {
    return {TypeMetadataDiagnosticsStatus::driver_reference_family_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (row.type_name.empty()) {
    return {TypeMetadataDiagnosticsStatus::driver_type_name_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (row.precision < 0 || row.scale < 0 || row.display_size < 0) {
    return {TypeMetadataDiagnosticsStatus::driver_numeric_metadata_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!DriverMetadataNullableIsValid(row.nullable) ||
      !DriverMetadataSignednessIsValid(row.signedness) ||
      !DriverMetadataCaseSensitivityIsValid(row.case_sensitive) ||
      !DriverMetadataSearchabilityIsValid(row.searchable) ||
      !TypeMetadataCompatibilityClassIsValid(row.compatibility_class) ||
      !TypeMetadataExposureClassIsValid(row.exposure_class) ||
      !TypeMetadataDisclosureClassIsValid(row.visibility_class) ||
      !TypeMetadataRedactionStateIsValid(row.redaction_state)) {
    return {TypeMetadataDiagnosticsStatus::driver_policy_enum_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if ((row.array_rank != 0 ||
       row.descriptor.family == ExecutionTypeFamily::structured ||
       row.descriptor.family == ExecutionTypeFamily::document) &&
      ExecutionDataPacketUuidIsNil(row.element_descriptor_uuid) &&
      row.descriptor.container_rank != 0) {
    return {TypeMetadataDiagnosticsStatus::driver_container_element_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!TypeMetadataCompatibilityClassExecutable(row.compatibility_class) &&
      row.unsupported_reason.empty()) {
    return {TypeMetadataDiagnosticsStatus::unsupported_reason_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(row.diagnostic_policy_ref)) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_policy_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (row.definition_hash.empty()) {
    return {TypeMetadataDiagnosticsStatus::driver_definition_hash_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!row.canonical_descriptor_authority_preserved ||
      !row.reference_name_not_authority || !row.protocol_code_not_authority ||
      !row.derived_without_execution) {
    return {TypeMetadataDiagnosticsStatus::
                driver_false_authority_guard_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  return {};
}

inline bool TypeMetadataDiagnosticsHasDriverFamily(
    const TypeMetadataDiagnosticsDriverContract& contract,
    DriverMetadataFamily family) noexcept {
  for (const auto& row : contract.driver_metadata_rows) {
    if (row.driver_family == family) {
      return true;
    }
  }
  return false;
}

inline TypeMetadataDiagnosticsValidationResult
ValidateTypeMetadataDiagnosticPayload(
    const TypeMetadataDiagnosticPayload& diagnostic,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(diagnostic.diagnostic_uuid)) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (diagnostic.scratchbird_code.empty() || diagnostic.severity.empty()) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_code_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(diagnostic.descriptor_uuid)) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!DriverMetadataFamilyIsValid(diagnostic.driver_family) ||
      !TypeMetadataCompatibilityClassIsValid(
          diagnostic.compatibility_class) ||
      !TypeMetadataDiagnosticPhaseIsValid(diagnostic.source_phase) ||
      !TypeMetadataDisclosureClassIsValid(diagnostic.disclosure_class) ||
      !TypeMetadataRedactionStateIsValid(diagnostic.redaction_state)) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_policy_enum_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (diagnostic.stable_search_key.empty()) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_search_key_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!diagnostic.protected_values_redacted ||
      !diagnostic.hidden_metadata_redacted ||
      !diagnostic.bridge_targets_redacted ||
      !diagnostic.encryption_material_redacted ||
      !diagnostic.raw_paths_redacted ||
      !diagnostic.privileged_catalog_rows_redacted) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_redaction_failed,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!diagnostic.reference_message_vector.empty() &&
      !diagnostic.reference_message_permitted) {
    return {TypeMetadataDiagnosticsStatus::reference_message_not_permitted,
            ExecutionDataPacketStatus::ok, row_index};
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult ValidateBackupRestoreTypeProfile(
    const BackupRestoreTypeProfile& profile,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(profile.profile_uuid)) {
    return {TypeMetadataDiagnosticsStatus::backup_profile_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(profile.descriptor_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.canonical_encoding_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.logical_rendering_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.descriptor_snapshot_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.resource_profile_uuid) ||
      profile.resource_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::backup_profile_identity_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!profile.logical_backup_supported || !profile.restore_admission_supported ||
      !profile.descriptor_snapshot_required ||
      !profile.version_migration_declared ||
      !profile.unsupported_values_refuse ||
      !profile.protected_value_policy_declared ||
      !profile.opaque_locator_policy_declared ||
      profile.conformance_key.empty()) {
    return {TypeMetadataDiagnosticsStatus::backup_profile_policy_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (profile.cpp_udr_dependency_required &&
      !TypeMetadataDiagnosticsCppUdrHookAdmitted(profile.cpp_udr_dependency)) {
    return {TypeMetadataDiagnosticsStatus::backup_profile_udr_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  const auto diagnostic_result =
      ValidateTypeMetadataDiagnosticPayload(profile.diagnostic, row_index);
  if (!diagnostic_result.ok()) {
    return diagnostic_result;
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult ValidateReplicationTypeProfile(
    const ReplicationTypeProfile& profile,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(profile.profile_uuid)) {
    return {TypeMetadataDiagnosticsStatus::replication_profile_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(profile.descriptor_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.delta_encoding_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.full_value_encoding_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.resource_profile_uuid) ||
      profile.resource_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::replication_profile_identity_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!profile.replication_safe || !profile.delta_encoding_declared ||
      !profile.full_value_replacement_declared ||
      !profile.element_delta_policy_declared ||
      !profile.ordering_causal_metadata_declared ||
      !profile.version_compatibility_declared ||
      !profile.conflict_behavior_declared ||
      !profile.fails_closed_when_unproven || profile.conformance_key.empty()) {
    return {TypeMetadataDiagnosticsStatus::replication_profile_policy_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  const auto diagnostic_result =
      ValidateTypeMetadataDiagnosticPayload(profile.diagnostic, row_index);
  if (!diagnostic_result.ok()) {
    return diagnostic_result;
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult
ValidateClusterTypeTransportProfile(
    const ClusterTypeTransportProfile& profile,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(profile.profile_uuid)) {
    return {TypeMetadataDiagnosticsStatus::cluster_profile_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(profile.descriptor_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.transport_encoding_uuid) ||
      ExecutionDataPacketUuidIsNil(profile.resource_profile_uuid) ||
      profile.resource_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::cluster_profile_identity_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!profile.compression_policy_declared ||
      !profile.encryption_policy_declared ||
      !profile.protected_value_policy_declared ||
      !profile.resource_negotiation_declared ||
      !profile.fallback_refusal_declared || !profile.route_policy_declared ||
      !profile.rejects_incompatible_descriptors ||
      !profile.rejects_incompatible_resources ||
      !profile.rejects_incompatible_udrs ||
      !profile.rejects_incompatible_policies ||
      profile.conformance_key.empty()) {
    return {TypeMetadataDiagnosticsStatus::cluster_profile_policy_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (profile.cluster_authority_recorded &&
      !profile.cluster_governance_present) {
    return {TypeMetadataDiagnosticsStatus::
                cluster_authority_without_governance,
            ExecutionDataPacketStatus::ok, row_index};
  }
  const auto diagnostic_result =
      ValidateTypeMetadataDiagnosticPayload(profile.diagnostic, row_index);
  if (!diagnostic_result.ok()) {
    return diagnostic_result;
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult
ValidateUnsupportedDegradedTypeContract(
    const UnsupportedDegradedTypeContract& contract,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(contract.contract_uuid)) {
    return {TypeMetadataDiagnosticsStatus::unsupported_contract_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (ExecutionDataPacketUuidIsNil(contract.descriptor_uuid) ||
      !TypeMetadataCompatibilityClassIsValid(contract.compatibility_class) ||
      contract.diagnostic_code.empty() || contract.metadata_status.empty() ||
      !contract.metadata_visible || !contract.diagnostic_visible) {
    return {TypeMetadataDiagnosticsStatus::
                unsupported_contract_diagnostic_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!TypeMetadataCompatibilityClassExecutable(contract.compatibility_class) &&
      contract.unsupported_reason.empty()) {
    return {TypeMetadataDiagnosticsStatus::unsupported_reason_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (!contract.silent_fallback_impossible ||
      !contract.non_executable_when_not_native) {
    return {TypeMetadataDiagnosticsStatus::unsupported_contract_silent_fallback,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (contract.compatibility_class == TypeMetadataCompatibilityClass::bridge_only &&
      (!contract.bridge_availability_checked ||
       !contract.cpp_bridge_required_for_bridge_only ||
       !contract.non_cpp_bridge_forbidden ||
       !TypeMetadataDiagnosticsCppUdrHookAdmitted(contract.bridge_hook))) {
    return {TypeMetadataDiagnosticsStatus::unsupported_contract_bridge_invalid,
            ExecutionDataPacketStatus::ok, row_index};
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult ValidateTypeTestingCorpus(
    const TypeTestingCorpusDescriptor& corpus,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(corpus.corpus_uuid) ||
      ExecutionDataPacketUuidIsNil(corpus.descriptor_uuid)) {
    return {TypeMetadataDiagnosticsStatus::testing_corpus_uuid_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (corpus.conformance_manifest_hash.empty()) {
    return {TypeMetadataDiagnosticsStatus::testing_corpus_manifest_required,
            ExecutionDataPacketStatus::ok, row_index};
  }
  if (corpus.implemented_reference_mapping &&
      (ExecutionDataPacketUuidIsNil(corpus.reference_mapping_uuid) ||
       !corpus.creation_metadata_case || !corpus.literal_bind_case ||
       !corpus.cast_case || !corpus.operation_case || !corpus.index_case ||
       !corpus.statistics_case || !corpus.backup_restore_case ||
       !corpus.replication_case || !corpus.driver_metadata_case ||
       !corpus.diagnostics_case)) {
    return {TypeMetadataDiagnosticsStatus::testing_corpus_case_missing,
            ExecutionDataPacketStatus::ok, row_index};
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult ValidateTypeMetadataCacheKey(
    const TypeMetadataCacheKeyDescriptor& cache_key) {
  if (ExecutionDataPacketUuidIsNil(cache_key.database_uuid) ||
      ExecutionDataPacketUuidIsNil(cache_key.descriptor_uuid) ||
      ExecutionDataPacketUuidIsNil(cache_key.policy_uuid) ||
      cache_key.capability_profile_definition_hash.empty()) {
    return {TypeMetadataDiagnosticsStatus::cache_key_dependency_missing};
  }
  if (cache_key.schema_epoch == 0 || cache_key.security_epoch == 0 ||
      cache_key.resource_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::cache_key_epoch_missing};
  }
  if (!DriverMetadataFamilyIsValid(cache_key.driver_family) ||
      !cache_key.includes_database || !cache_key.includes_descriptor ||
      !cache_key.includes_driver_family || !cache_key.includes_schema_epoch ||
      !cache_key.includes_security_epoch ||
      !cache_key.includes_resource_epoch ||
      !cache_key.includes_definition_hashes || !cache_key.includes_policy_uuid) {
    return {TypeMetadataDiagnosticsStatus::cache_key_dependency_missing};
  }
  if (!ExecutionDataPacketUuidIsNil(cache_key.domain_uuid) &&
      (!cache_key.includes_domain_when_present ||
       cache_key.domain_stack_hash.empty())) {
    return {TypeMetadataDiagnosticsStatus::cache_key_dependency_missing};
  }
  if (!cache_key.reference_family.empty() &&
      (!cache_key.includes_reference_when_present ||
       cache_key.reference_version_profile.empty())) {
    return {TypeMetadataDiagnosticsStatus::cache_key_dependency_missing};
  }
  if (!cache_key.parser_family_untrusted_context_only) {
    return {TypeMetadataDiagnosticsStatus::cache_key_parser_authority};
  }
  if (cache_key.metadata_query_causes_execution ||
      cache_key.metadata_query_loads_udr ||
      cache_key.metadata_query_mutates_storage ||
      cache_key.metadata_query_executes_parser_sql) {
    return {TypeMetadataDiagnosticsStatus::metadata_query_side_effect};
  }
  return {};
}

inline TypeMetadataDiagnosticsValidationResult
ValidateTypeMetadataDiagnosticsDriverContract(
    const TypeMetadataDiagnosticsDriverContract& contract) {
  if (ExecutionDataPacketUuidIsNil(contract.contract_uuid)) {
    return {TypeMetadataDiagnosticsStatus::contract_uuid_required};
  }
  if (contract.contract_epoch == 0) {
    return {TypeMetadataDiagnosticsStatus::contract_epoch_required};
  }
  if (contract.stable_name.empty()) {
    return {TypeMetadataDiagnosticsStatus::stable_name_required};
  }
  if (!contract.descriptor_authoritative) {
    return {TypeMetadataDiagnosticsStatus::descriptor_not_authoritative};
  }
  if (!contract.parser_independent) {
    return {TypeMetadataDiagnosticsStatus::descriptor_parser_dependent};
  }
  const auto catalog_result =
      ValidateTypeMetadataCatalogExposure(contract.catalog_exposure);
  if (!catalog_result.ok()) {
    return catalog_result;
  }
  if (contract.driver_metadata_rows.empty()) {
    return {TypeMetadataDiagnosticsStatus::driver_rows_required};
  }
  constexpr DriverMetadataFamily required_driver_families[] = {
      DriverMetadataFamily::odbc, DriverMetadataFamily::jdbc,
      DriverMetadataFamily::dotnet, DriverMetadataFamily::native,
      DriverMetadataFamily::reference_specific};
  for (const auto family : required_driver_families) {
    if (!TypeMetadataDiagnosticsHasDriverFamily(contract, family)) {
      return {TypeMetadataDiagnosticsStatus::driver_family_coverage_missing};
    }
  }
  for (std::size_t row_index = 0;
       row_index < contract.driver_metadata_rows.size(); ++row_index) {
    const auto row_result = ValidateDriverTypeMetadataDescriptor(
        contract.driver_metadata_rows[row_index], row_index);
    if (!row_result.ok()) {
      return row_result;
    }
  }
  if (contract.diagnostic_payloads.empty()) {
    return {TypeMetadataDiagnosticsStatus::diagnostic_payload_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.diagnostic_payloads.size(); ++row_index) {
    const auto diagnostic_result = ValidateTypeMetadataDiagnosticPayload(
        contract.diagnostic_payloads[row_index], row_index);
    if (!diagnostic_result.ok()) {
      return diagnostic_result;
    }
  }
  if (contract.backup_restore_profiles.empty()) {
    return {TypeMetadataDiagnosticsStatus::backup_profile_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.backup_restore_profiles.size(); ++row_index) {
    const auto profile_result = ValidateBackupRestoreTypeProfile(
        contract.backup_restore_profiles[row_index], row_index);
    if (!profile_result.ok()) {
      return profile_result;
    }
  }
  if (contract.replication_profiles.empty()) {
    return {TypeMetadataDiagnosticsStatus::replication_profile_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.replication_profiles.size(); ++row_index) {
    const auto profile_result = ValidateReplicationTypeProfile(
        contract.replication_profiles[row_index], row_index);
    if (!profile_result.ok()) {
      return profile_result;
    }
  }
  if (contract.cluster_transport_profiles.empty()) {
    return {TypeMetadataDiagnosticsStatus::cluster_profile_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.cluster_transport_profiles.size(); ++row_index) {
    const auto profile_result = ValidateClusterTypeTransportProfile(
        contract.cluster_transport_profiles[row_index], row_index);
    if (!profile_result.ok()) {
      return profile_result;
    }
  }
  if (contract.unsupported_contracts.empty()) {
    return {TypeMetadataDiagnosticsStatus::unsupported_contract_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.unsupported_contracts.size(); ++row_index) {
    const auto unsupported_result = ValidateUnsupportedDegradedTypeContract(
        contract.unsupported_contracts[row_index], row_index);
    if (!unsupported_result.ok()) {
      return unsupported_result;
    }
  }
  if (contract.testing_corpus.empty()) {
    return {TypeMetadataDiagnosticsStatus::testing_corpus_required};
  }
  for (std::size_t row_index = 0;
       row_index < contract.testing_corpus.size(); ++row_index) {
    const auto corpus_result =
        ValidateTypeTestingCorpus(contract.testing_corpus[row_index],
                                  row_index);
    if (!corpus_result.ok()) {
      return corpus_result;
    }
  }
  const auto cache_result = ValidateTypeMetadataCacheKey(contract.cache_key);
  if (!cache_result.ok()) {
    return cache_result;
  }
  if (!contract.local_metrics_root_declared) {
    return {TypeMetadataDiagnosticsStatus::local_metrics_root_required};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.type_metadata.catalog_query_count",
      "sys.metrics.type_metadata.driver_metadata_query_count",
      "sys.metrics.type_metadata.reference_metadata_query_count",
      "sys.metrics.type_metadata.hidden_row_count",
      "sys.metrics.type_metadata.redacted_field_count",
      "sys.metrics.type_metadata.unsupported_type_count",
      "sys.metrics.type_metadata.deferred_type_count",
      "sys.metrics.type_metadata.bridge_only_type_count",
      "sys.metrics.type_metadata.degraded_type_count",
      "sys.metrics.type_metadata.diagnostic_render_count",
      "sys.metrics.type_metadata.diagnostic_redaction_count",
      "sys.metrics.type_metadata.cache_hit_count",
      "sys.metrics.type_metadata.cache_stale_rejection_count",
      "sys.metrics.type_metadata.profile_missing_count"};
  for (std::string_view metric : required_metrics) {
    if (!TypeMetadataDiagnosticsMetricPresent(contract, metric)) {
      return {TypeMetadataDiagnosticsStatus::local_metric_missing};
    }
  }
  if (!contract.cluster_metrics_guarded_by_cluster_governance) {
    return {TypeMetadataDiagnosticsStatus::cluster_metrics_guard_required};
  }
  return {};
}

// SEARCH_KEY: EDC-DETAIL-CLOSURE-MATRICES
// SEARCH_KEY: EDC-CANONICAL-TYPE-FAMILY-INVENTORY
// SEARCH_KEY: EDC-REFERENCE-TYPE-COVERAGE-MATRIX
// SEARCH_KEY: EDC-OPERATION-REGISTRY-SEED-MATRIX
// SEARCH_KEY: EDC-CAST-REGISTRY-SEED-MATRIX
// SEARCH_KEY: EDC-AGGREGATE-WINDOW-SEED-MATRIX
// SEARCH_KEY: EDC-INDEX-FAMILY-COMPATIBILITY-MATRIX
// SEARCH_KEY: EDC-STATISTICS-SELECTIVITY-SEED-MATRIX
// SEARCH_KEY: EDC-DRIVER-METADATA-SEED-MATRIX
// SEARCH_KEY: EDC-BACKUP-REPLICATION-TRANSPORT-MATRIX
// SEARCH_KEY: EDC-SYSTEM-CATALOG-SEED-CONTRACTS
// SEARCH_KEY: EDC-CONFORMANCE-CORPUS-SEED-MATRIX
enum class ExecutionDetailClosureMatrixKind : std::uint8_t {
  canonical_type_family = 0,
  reference_type_coverage = 1,
  operation_registry = 2,
  cast_registry = 3,
  aggregate_window = 4,
  index_family_compatibility = 5,
  statistics_selectivity = 6,
  driver_metadata = 7,
  backup_replication_transport = 8,
  system_catalog = 9,
  conformance_corpus = 10
};

constexpr bool ExecutionDetailClosureMatrixKindIsValid(
    ExecutionDetailClosureMatrixKind kind) noexcept {
  switch (kind) {
    case ExecutionDetailClosureMatrixKind::canonical_type_family:
    case ExecutionDetailClosureMatrixKind::reference_type_coverage:
    case ExecutionDetailClosureMatrixKind::operation_registry:
    case ExecutionDetailClosureMatrixKind::cast_registry:
    case ExecutionDetailClosureMatrixKind::aggregate_window:
    case ExecutionDetailClosureMatrixKind::index_family_compatibility:
    case ExecutionDetailClosureMatrixKind::statistics_selectivity:
    case ExecutionDetailClosureMatrixKind::driver_metadata:
    case ExecutionDetailClosureMatrixKind::backup_replication_transport:
    case ExecutionDetailClosureMatrixKind::system_catalog:
    case ExecutionDetailClosureMatrixKind::conformance_corpus:
      return true;
  }
  return false;
}

constexpr std::string_view ExecutionDetailClosureMatrixKindSearchKey(
    ExecutionDetailClosureMatrixKind kind) noexcept {
  switch (kind) {
    case ExecutionDetailClosureMatrixKind::canonical_type_family:
      return "EDC-CANONICAL-TYPE-FAMILY-INVENTORY";
    case ExecutionDetailClosureMatrixKind::reference_type_coverage:
      return "EDC-REFERENCE-TYPE-COVERAGE-MATRIX";
    case ExecutionDetailClosureMatrixKind::operation_registry:
      return "EDC-OPERATION-REGISTRY-SEED-MATRIX";
    case ExecutionDetailClosureMatrixKind::cast_registry:
      return "EDC-CAST-REGISTRY-SEED-MATRIX";
    case ExecutionDetailClosureMatrixKind::aggregate_window:
      return "EDC-AGGREGATE-WINDOW-SEED-MATRIX";
    case ExecutionDetailClosureMatrixKind::index_family_compatibility:
      return "EDC-INDEX-FAMILY-COMPATIBILITY-MATRIX";
    case ExecutionDetailClosureMatrixKind::statistics_selectivity:
      return "EDC-STATISTICS-SELECTIVITY-SEED-MATRIX";
    case ExecutionDetailClosureMatrixKind::driver_metadata:
      return "EDC-DRIVER-METADATA-SEED-MATRIX";
    case ExecutionDetailClosureMatrixKind::backup_replication_transport:
      return "EDC-BACKUP-REPLICATION-TRANSPORT-MATRIX";
    case ExecutionDetailClosureMatrixKind::system_catalog:
      return "EDC-SYSTEM-CATALOG-SEED-CONTRACTS";
    case ExecutionDetailClosureMatrixKind::conformance_corpus:
      return "EDC-CONFORMANCE-CORPUS-SEED-MATRIX";
  }
  return "";
}

enum class ExecutionDetailClosureCoverageStatus : std::uint8_t {
  complete = 0,
  partial = 1,
  deferred = 2,
  superseded = 3,
  unsupported_by_policy = 4,
  implementation_pending = 5
};

constexpr bool ExecutionDetailClosureCoverageStatusIsValid(
    ExecutionDetailClosureCoverageStatus status) noexcept {
  switch (status) {
    case ExecutionDetailClosureCoverageStatus::complete:
    case ExecutionDetailClosureCoverageStatus::partial:
    case ExecutionDetailClosureCoverageStatus::deferred:
    case ExecutionDetailClosureCoverageStatus::superseded:
    case ExecutionDetailClosureCoverageStatus::unsupported_by_policy:
    case ExecutionDetailClosureCoverageStatus::implementation_pending:
      return true;
  }
  return false;
}

enum class ExecutionDetailClosureRowScope : std::uint8_t {
  native = 0,
  reference = 1,
  parser = 2,
  driver = 3,
  engine = 4,
  storage = 5,
  index = 6,
  optimizer = 7,
  backup = 8,
  replication = 9,
  cluster = 10,
  catalog = 11,
  diagnostic = 12,
  conformance = 13,
  cross_subsystem = 14
};

constexpr bool ExecutionDetailClosureRowScopeIsValid(
    ExecutionDetailClosureRowScope scope) noexcept {
  switch (scope) {
    case ExecutionDetailClosureRowScope::native:
    case ExecutionDetailClosureRowScope::reference:
    case ExecutionDetailClosureRowScope::parser:
    case ExecutionDetailClosureRowScope::driver:
    case ExecutionDetailClosureRowScope::engine:
    case ExecutionDetailClosureRowScope::storage:
    case ExecutionDetailClosureRowScope::index:
    case ExecutionDetailClosureRowScope::optimizer:
    case ExecutionDetailClosureRowScope::backup:
    case ExecutionDetailClosureRowScope::replication:
    case ExecutionDetailClosureRowScope::cluster:
    case ExecutionDetailClosureRowScope::catalog:
    case ExecutionDetailClosureRowScope::diagnostic:
    case ExecutionDetailClosureRowScope::conformance:
    case ExecutionDetailClosureRowScope::cross_subsystem:
      return true;
  }
  return false;
}

enum class ExecutionDetailClosureCompletionState : std::uint8_t {
  complete = 0,
  partial = 1,
  deferred = 2,
  unsupported_by_version = 3,
  unsupported_by_policy = 4,
  render_only = 5,
  bridge_only = 6,
  preserve_only = 7,
  implementation_pending = 8,
  superseded = 9
};

constexpr bool ExecutionDetailClosureCompletionStateIsValid(
    ExecutionDetailClosureCompletionState state) noexcept {
  switch (state) {
    case ExecutionDetailClosureCompletionState::complete:
    case ExecutionDetailClosureCompletionState::partial:
    case ExecutionDetailClosureCompletionState::deferred:
    case ExecutionDetailClosureCompletionState::unsupported_by_version:
    case ExecutionDetailClosureCompletionState::unsupported_by_policy:
    case ExecutionDetailClosureCompletionState::render_only:
    case ExecutionDetailClosureCompletionState::bridge_only:
    case ExecutionDetailClosureCompletionState::preserve_only:
    case ExecutionDetailClosureCompletionState::implementation_pending:
    case ExecutionDetailClosureCompletionState::superseded:
      return true;
  }
  return false;
}

constexpr bool ExecutionDetailClosureCompletionRequiresDiagnostic(
    ExecutionDetailClosureCompletionState state) noexcept {
  return state == ExecutionDetailClosureCompletionState::deferred ||
         state == ExecutionDetailClosureCompletionState::unsupported_by_version ||
         state == ExecutionDetailClosureCompletionState::unsupported_by_policy ||
         state == ExecutionDetailClosureCompletionState::render_only ||
         state == ExecutionDetailClosureCompletionState::bridge_only ||
         state == ExecutionDetailClosureCompletionState::preserve_only ||
         state == ExecutionDetailClosureCompletionState::implementation_pending;
}

enum class ExecutionDetailClosureSurfaceKind : std::uint8_t {
  descriptor = 0,
  domain = 1,
  literal_bind = 2,
  cast = 3,
  operation = 4,
  aggregate_window = 5,
  index = 6,
  statistics = 7,
  driver_metadata = 8,
  backup_restore = 9,
  replication = 10,
  cluster_transport = 11,
  storage_encoding = 12,
  wire_rendering = 13,
  diagnostic = 14,
  security = 15,
  metrics = 16,
  catalog = 17,
  conformance = 18,
  implementation_trace = 19,
  documentation = 20
};

constexpr bool ExecutionDetailClosureSurfaceKindIsValid(
    ExecutionDetailClosureSurfaceKind kind) noexcept {
  switch (kind) {
    case ExecutionDetailClosureSurfaceKind::descriptor:
    case ExecutionDetailClosureSurfaceKind::domain:
    case ExecutionDetailClosureSurfaceKind::literal_bind:
    case ExecutionDetailClosureSurfaceKind::cast:
    case ExecutionDetailClosureSurfaceKind::operation:
    case ExecutionDetailClosureSurfaceKind::aggregate_window:
    case ExecutionDetailClosureSurfaceKind::index:
    case ExecutionDetailClosureSurfaceKind::statistics:
    case ExecutionDetailClosureSurfaceKind::driver_metadata:
    case ExecutionDetailClosureSurfaceKind::backup_restore:
    case ExecutionDetailClosureSurfaceKind::replication:
    case ExecutionDetailClosureSurfaceKind::cluster_transport:
    case ExecutionDetailClosureSurfaceKind::storage_encoding:
    case ExecutionDetailClosureSurfaceKind::wire_rendering:
    case ExecutionDetailClosureSurfaceKind::diagnostic:
    case ExecutionDetailClosureSurfaceKind::security:
    case ExecutionDetailClosureSurfaceKind::metrics:
    case ExecutionDetailClosureSurfaceKind::catalog:
    case ExecutionDetailClosureSurfaceKind::conformance:
    case ExecutionDetailClosureSurfaceKind::implementation_trace:
    case ExecutionDetailClosureSurfaceKind::documentation:
      return true;
  }
  return false;
}

enum class ExecutionDetailClosureSurfaceStatus : std::uint8_t {
  complete = 0,
  partial = 1,
  deferred = 2,
  unsupported_by_version = 3,
  unsupported_by_policy = 4,
  not_applicable = 5,
  implementation_pending = 6
};

constexpr bool ExecutionDetailClosureSurfaceStatusIsValid(
    ExecutionDetailClosureSurfaceStatus status) noexcept {
  switch (status) {
    case ExecutionDetailClosureSurfaceStatus::complete:
    case ExecutionDetailClosureSurfaceStatus::partial:
    case ExecutionDetailClosureSurfaceStatus::deferred:
    case ExecutionDetailClosureSurfaceStatus::unsupported_by_version:
    case ExecutionDetailClosureSurfaceStatus::unsupported_by_policy:
    case ExecutionDetailClosureSurfaceStatus::not_applicable:
    case ExecutionDetailClosureSurfaceStatus::implementation_pending:
      return true;
  }
  return false;
}

constexpr bool ExecutionDetailClosureSurfaceClosed(
    ExecutionDetailClosureSurfaceStatus status) noexcept {
  return status == ExecutionDetailClosureSurfaceStatus::complete ||
         status == ExecutionDetailClosureSurfaceStatus::not_applicable ||
         status == ExecutionDetailClosureSurfaceStatus::deferred ||
         status == ExecutionDetailClosureSurfaceStatus::unsupported_by_version ||
         status == ExecutionDetailClosureSurfaceStatus::unsupported_by_policy;
}

enum class ExecutionDetailClosureClaimScope : std::uint8_t {
  specification_ready = 0,
  implementation_ready = 1,
  release_ready = 2,
  reference_compatible = 3,
  driver_visible = 4,
  backup_safe = 5,
  replication_safe = 6,
  cluster_transport_safe = 7,
  native_or_better = 8
};

constexpr bool ExecutionDetailClosureClaimScopeIsValid(
    ExecutionDetailClosureClaimScope scope) noexcept {
  switch (scope) {
    case ExecutionDetailClosureClaimScope::specification_ready:
    case ExecutionDetailClosureClaimScope::implementation_ready:
    case ExecutionDetailClosureClaimScope::release_ready:
    case ExecutionDetailClosureClaimScope::reference_compatible:
    case ExecutionDetailClosureClaimScope::driver_visible:
    case ExecutionDetailClosureClaimScope::backup_safe:
    case ExecutionDetailClosureClaimScope::replication_safe:
    case ExecutionDetailClosureClaimScope::cluster_transport_safe:
    case ExecutionDetailClosureClaimScope::native_or_better:
      return true;
  }
  return false;
}

constexpr bool ExecutionDetailClosureClaimRequiresImplementationTrace(
    ExecutionDetailClosureClaimScope scope) noexcept {
  return scope != ExecutionDetailClosureClaimScope::specification_ready;
}

constexpr bool ExecutionDetailClosureClaimRequiresConformance(
    ExecutionDetailClosureClaimScope scope) noexcept {
  return scope == ExecutionDetailClosureClaimScope::release_ready ||
         scope == ExecutionDetailClosureClaimScope::reference_compatible ||
         scope == ExecutionDetailClosureClaimScope::driver_visible ||
         scope == ExecutionDetailClosureClaimScope::backup_safe ||
         scope == ExecutionDetailClosureClaimScope::replication_safe ||
         scope == ExecutionDetailClosureClaimScope::cluster_transport_safe ||
         scope == ExecutionDetailClosureClaimScope::native_or_better;
}

enum class ExecutionDetailClosureClaimStatus : std::uint8_t {
  go = 0,
  no_go = 1,
  deferred = 2,
  forbidden = 3,
  internal_only = 4
};

constexpr bool ExecutionDetailClosureClaimStatusIsValid(
    ExecutionDetailClosureClaimStatus status) noexcept {
  switch (status) {
    case ExecutionDetailClosureClaimStatus::go:
    case ExecutionDetailClosureClaimStatus::no_go:
    case ExecutionDetailClosureClaimStatus::deferred:
    case ExecutionDetailClosureClaimStatus::forbidden:
    case ExecutionDetailClosureClaimStatus::internal_only:
      return true;
  }
  return false;
}

enum class ExecutionDetailClosureSupersessionReason : std::uint8_t {
  expanded_detail = 0,
  contradiction_fix = 1,
  split_row = 2,
  merged_row = 3,
  renamed_subject = 4,
  moved_to_reference_matrix = 5,
  moved_to_chapter_15 = 6,
  human_decision = 7
};

constexpr bool ExecutionDetailClosureSupersessionReasonIsValid(
    ExecutionDetailClosureSupersessionReason reason) noexcept {
  switch (reason) {
    case ExecutionDetailClosureSupersessionReason::expanded_detail:
    case ExecutionDetailClosureSupersessionReason::contradiction_fix:
    case ExecutionDetailClosureSupersessionReason::split_row:
    case ExecutionDetailClosureSupersessionReason::merged_row:
    case ExecutionDetailClosureSupersessionReason::renamed_subject:
    case ExecutionDetailClosureSupersessionReason::moved_to_reference_matrix:
    case ExecutionDetailClosureSupersessionReason::moved_to_chapter_15:
    case ExecutionDetailClosureSupersessionReason::human_decision:
      return true;
  }
  return false;
}

enum class ExecutionDetailClosureSupersessionDecision : std::uint8_t {
  accepted = 0,
  rejected = 1,
  deferred = 2,
  human_review_required = 3
};

constexpr bool ExecutionDetailClosureSupersessionDecisionIsValid(
    ExecutionDetailClosureSupersessionDecision decision) noexcept {
  switch (decision) {
    case ExecutionDetailClosureSupersessionDecision::accepted:
    case ExecutionDetailClosureSupersessionDecision::rejected:
    case ExecutionDetailClosureSupersessionDecision::deferred:
    case ExecutionDetailClosureSupersessionDecision::human_review_required:
      return true;
  }
  return false;
}

struct ExecutionDetailClosureMatrixRecord {
  Uuid matrix_uuid{};
  std::string matrix_name;
  std::string matrix_search_key;
  ExecutionDetailClosureMatrixKind matrix_kind =
      ExecutionDetailClosureMatrixKind::canonical_type_family;
  std::string controlling_appendix_set_hash;
  std::string minimum_inventory_hash;
  std::vector<std::string> required_seed_subjects;
  Uuid successor_matrix_uuid{};
  ExecutionDetailClosureCoverageStatus coverage_status =
      ExecutionDetailClosureCoverageStatus::implementation_pending;
  std::string conformance_manifest_hash;
  Uuid implementation_trace_uuid{};
  std::string matrix_hash;
};

struct ExecutionDetailClosureRowRecord {
  Uuid matrix_row_uuid{};
  Uuid matrix_uuid{};
  std::string row_search_key;
  std::string row_subject;
  ExecutionDetailClosureRowScope row_scope =
      ExecutionDetailClosureRowScope::cross_subsystem;
  std::string representation_or_status;
  std::string controlling_spec_path;
  std::string controlling_search_key;
  std::string required_surface_set_hash;
  std::string completed_surface_set_hash;
  ExecutionDetailClosureCompletionState completion_state =
      ExecutionDetailClosureCompletionState::implementation_pending;
  std::string diagnostic_code;
  std::string conformance_case_set_hash;
  Uuid implementation_trace_uuid{};
  std::string row_hash;
};

struct ExecutionDetailClosureSurfaceRecord {
  Uuid surface_uuid{};
  Uuid matrix_row_uuid{};
  ExecutionDetailClosureSurfaceKind surface_kind =
      ExecutionDetailClosureSurfaceKind::descriptor;
  ExecutionDetailClosureSurfaceStatus surface_status =
      ExecutionDetailClosureSurfaceStatus::implementation_pending;
  std::string owning_spec_path;
  std::string owning_search_key;
  std::string evidence_hash;
  std::string failure_diagnostic_code;
  std::string surface_hash;
};

struct ExecutionDetailClosureClaimRecord {
  Uuid closure_claim_uuid{};
  Uuid matrix_row_uuid{};
  ExecutionDetailClosureClaimScope claim_scope =
      ExecutionDetailClosureClaimScope::specification_ready;
  ExecutionDetailClosureClaimStatus claim_status =
      ExecutionDetailClosureClaimStatus::no_go;
  std::string required_evidence_hash;
  std::string provided_evidence_hash;
  Uuid waiver_uuid{};
  std::string decision_diagnostic_code;
  std::string claim_hash;
};

struct ExecutionDetailSupersessionRecord {
  Uuid supersession_uuid{};
  Uuid old_matrix_row_uuid{};
  Uuid new_matrix_row_uuid{};
  std::string old_search_key;
  std::string new_search_key;
  ExecutionDetailClosureSupersessionReason supersession_reason =
      ExecutionDetailClosureSupersessionReason::expanded_detail;
  ExecutionDetailClosureSupersessionDecision decision_status =
      ExecutionDetailClosureSupersessionDecision::human_review_required;
  std::string supersession_hash;
};

struct ExecutionDetailClosureRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string registry_name;
  std::string detail_closure_search_key;
  std::vector<ExecutionDetailClosureMatrixRecord> matrices;
  std::vector<ExecutionDetailClosureRowRecord> rows;
  std::vector<ExecutionDetailClosureSurfaceRecord> surfaces;
  std::vector<ExecutionDetailClosureClaimRecord> claims;
  std::vector<ExecutionDetailSupersessionRecord> supersessions;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool diagnostics_root_declared = true;
  bool local_metrics_root_declared = true;
};

enum class ExecutionDetailClosureStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  closure_search_key_required = 4,
  descriptor_not_authoritative = 5,
  parser_independent_required = 6,
  matrix_records_required = 7,
  row_records_required = 8,
  surface_records_required = 9,
  matrix_record_missing = 10,
  matrix_uuid_required = 11,
  matrix_identity_incomplete = 12,
  matrix_kind_invalid = 13,
  matrix_duplicate = 14,
  matrix_successor_unresolved = 15,
  seed_inventory_incomplete = 16,
  matrix_row_missing = 17,
  matrix_row_uuid_required = 18,
  matrix_row_identity_incomplete = 19,
  row_scope_invalid = 20,
  row_completion_state_invalid = 21,
  deferred_or_unsupported_diagnostic_missing = 22,
  row_surface_coverage_incomplete = 23,
  surface_record_missing = 24,
  surface_uuid_required = 25,
  surface_identity_incomplete = 26,
  surface_kind_invalid = 27,
  surface_status_invalid = 28,
  surface_evidence_missing = 29,
  closure_claim_missing = 30,
  claim_uuid_required = 31,
  claim_identity_incomplete = 32,
  claim_scope_invalid = 33,
  claim_status_invalid = 34,
  claim_evidence_incomplete = 35,
  conformance_case_missing = 36,
  implementation_trace_missing = 37,
  native_or_better_unproven = 38,
  supersession_record_missing = 39,
  supersession_uuid_required = 40,
  supersession_identity_incomplete = 41,
  supersession_policy_invalid = 42,
  supersession_not_accepted = 43,
  diagnostic_vector_missing = 44,
  local_metrics_root_required = 45,
  local_metric_missing = 46
};

constexpr std::string_view ExecutionDetailClosureStatusName(
    ExecutionDetailClosureStatus status) noexcept {
  switch (status) {
    case ExecutionDetailClosureStatus::ok:
      return "ok";
    case ExecutionDetailClosureStatus::registry_uuid_required:
      return "registry_uuid_required";
    case ExecutionDetailClosureStatus::registry_epoch_required:
      return "registry_epoch_required";
    case ExecutionDetailClosureStatus::registry_name_required:
      return "registry_name_required";
    case ExecutionDetailClosureStatus::closure_search_key_required:
      return "closure_search_key_required";
    case ExecutionDetailClosureStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ExecutionDetailClosureStatus::parser_independent_required:
      return "parser_independent_required";
    case ExecutionDetailClosureStatus::matrix_records_required:
      return "matrix_records_required";
    case ExecutionDetailClosureStatus::row_records_required:
      return "row_records_required";
    case ExecutionDetailClosureStatus::surface_records_required:
      return "surface_records_required";
    case ExecutionDetailClosureStatus::matrix_record_missing:
      return "matrix_record_missing";
    case ExecutionDetailClosureStatus::matrix_uuid_required:
      return "matrix_uuid_required";
    case ExecutionDetailClosureStatus::matrix_identity_incomplete:
      return "matrix_identity_incomplete";
    case ExecutionDetailClosureStatus::matrix_kind_invalid:
      return "matrix_kind_invalid";
    case ExecutionDetailClosureStatus::matrix_duplicate:
      return "matrix_duplicate";
    case ExecutionDetailClosureStatus::matrix_successor_unresolved:
      return "matrix_successor_unresolved";
    case ExecutionDetailClosureStatus::seed_inventory_incomplete:
      return "seed_inventory_incomplete";
    case ExecutionDetailClosureStatus::matrix_row_missing:
      return "matrix_row_missing";
    case ExecutionDetailClosureStatus::matrix_row_uuid_required:
      return "matrix_row_uuid_required";
    case ExecutionDetailClosureStatus::matrix_row_identity_incomplete:
      return "matrix_row_identity_incomplete";
    case ExecutionDetailClosureStatus::row_scope_invalid:
      return "row_scope_invalid";
    case ExecutionDetailClosureStatus::row_completion_state_invalid:
      return "row_completion_state_invalid";
    case ExecutionDetailClosureStatus::
        deferred_or_unsupported_diagnostic_missing:
      return "deferred_or_unsupported_diagnostic_missing";
    case ExecutionDetailClosureStatus::row_surface_coverage_incomplete:
      return "row_surface_coverage_incomplete";
    case ExecutionDetailClosureStatus::surface_record_missing:
      return "surface_record_missing";
    case ExecutionDetailClosureStatus::surface_uuid_required:
      return "surface_uuid_required";
    case ExecutionDetailClosureStatus::surface_identity_incomplete:
      return "surface_identity_incomplete";
    case ExecutionDetailClosureStatus::surface_kind_invalid:
      return "surface_kind_invalid";
    case ExecutionDetailClosureStatus::surface_status_invalid:
      return "surface_status_invalid";
    case ExecutionDetailClosureStatus::surface_evidence_missing:
      return "surface_evidence_missing";
    case ExecutionDetailClosureStatus::closure_claim_missing:
      return "closure_claim_missing";
    case ExecutionDetailClosureStatus::claim_uuid_required:
      return "claim_uuid_required";
    case ExecutionDetailClosureStatus::claim_identity_incomplete:
      return "claim_identity_incomplete";
    case ExecutionDetailClosureStatus::claim_scope_invalid:
      return "claim_scope_invalid";
    case ExecutionDetailClosureStatus::claim_status_invalid:
      return "claim_status_invalid";
    case ExecutionDetailClosureStatus::claim_evidence_incomplete:
      return "claim_evidence_incomplete";
    case ExecutionDetailClosureStatus::conformance_case_missing:
      return "conformance_case_missing";
    case ExecutionDetailClosureStatus::implementation_trace_missing:
      return "implementation_trace_missing";
    case ExecutionDetailClosureStatus::native_or_better_unproven:
      return "native_or_better_unproven";
    case ExecutionDetailClosureStatus::supersession_record_missing:
      return "supersession_record_missing";
    case ExecutionDetailClosureStatus::supersession_uuid_required:
      return "supersession_uuid_required";
    case ExecutionDetailClosureStatus::supersession_identity_incomplete:
      return "supersession_identity_incomplete";
    case ExecutionDetailClosureStatus::supersession_policy_invalid:
      return "supersession_policy_invalid";
    case ExecutionDetailClosureStatus::supersession_not_accepted:
      return "supersession_not_accepted";
    case ExecutionDetailClosureStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case ExecutionDetailClosureStatus::local_metrics_root_required:
      return "local_metrics_root_required";
    case ExecutionDetailClosureStatus::local_metric_missing:
      return "local_metric_missing";
  }
  return "unknown_status";
}

struct ExecutionDetailClosureValidationResult {
  ExecutionDetailClosureStatus status = ExecutionDetailClosureStatus::ok;
  std::size_t matrix_index = 0;
  std::size_t row_index = 0;
  std::size_t surface_index = 0;
  std::size_t claim_index = 0;
  std::size_t supersession_index = 0;

  bool ok() const noexcept {
    return status == ExecutionDetailClosureStatus::ok;
  }
};

inline const ExecutionDetailClosureMatrixRecord*
ExecutionDetailClosureFindMatrixByUuid(
    const ExecutionDetailClosureRegistry& registry,
    const Uuid& matrix_uuid) noexcept {
  for (const auto& matrix : registry.matrices) {
    if (ExecutionDataPacketUuidEquals(matrix.matrix_uuid, matrix_uuid)) {
      return &matrix;
    }
  }
  return nullptr;
}

inline const ExecutionDetailClosureRowRecord*
ExecutionDetailClosureFindRowByUuid(
    const ExecutionDetailClosureRegistry& registry,
    const Uuid& row_uuid) noexcept {
  for (const auto& row : registry.rows) {
    if (ExecutionDataPacketUuidEquals(row.matrix_row_uuid, row_uuid)) {
      return &row;
    }
  }
  return nullptr;
}

inline bool ExecutionDetailClosureHasMatrixKind(
    const ExecutionDetailClosureRegistry& registry,
    ExecutionDetailClosureMatrixKind kind) noexcept {
  for (const auto& matrix : registry.matrices) {
    if (matrix.matrix_kind == kind) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureHasMetric(
    const ExecutionDetailClosureRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureHasDiagnosticCode(
    const ExecutionDetailClosureRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

constexpr bool ExecutionDetailClosureSurfaceRequiredForMatrix(
    ExecutionDetailClosureMatrixKind matrix_kind,
    ExecutionDetailClosureSurfaceKind surface_kind) noexcept {
  switch (matrix_kind) {
    case ExecutionDetailClosureMatrixKind::canonical_type_family:
      return surface_kind == ExecutionDetailClosureSurfaceKind::descriptor ||
             surface_kind == ExecutionDetailClosureSurfaceKind::domain ||
             surface_kind == ExecutionDetailClosureSurfaceKind::operation ||
             surface_kind == ExecutionDetailClosureSurfaceKind::storage_encoding ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::documentation;
    case ExecutionDetailClosureMatrixKind::reference_type_coverage:
      return surface_kind == ExecutionDetailClosureSurfaceKind::descriptor ||
             surface_kind == ExecutionDetailClosureSurfaceKind::domain ||
             surface_kind == ExecutionDetailClosureSurfaceKind::literal_bind ||
             surface_kind == ExecutionDetailClosureSurfaceKind::cast ||
             surface_kind == ExecutionDetailClosureSurfaceKind::operation ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::documentation;
    case ExecutionDetailClosureMatrixKind::operation_registry:
      return surface_kind == ExecutionDetailClosureSurfaceKind::operation ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::cast_registry:
      return surface_kind == ExecutionDetailClosureSurfaceKind::cast ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::aggregate_window:
      return surface_kind == ExecutionDetailClosureSurfaceKind::aggregate_window ||
             surface_kind == ExecutionDetailClosureSurfaceKind::storage_encoding ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::index_family_compatibility:
      return surface_kind == ExecutionDetailClosureSurfaceKind::index ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::statistics_selectivity:
      return surface_kind == ExecutionDetailClosureSurfaceKind::statistics ||
             surface_kind == ExecutionDetailClosureSurfaceKind::metrics ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::driver_metadata:
      return surface_kind == ExecutionDetailClosureSurfaceKind::driver_metadata ||
             surface_kind == ExecutionDetailClosureSurfaceKind::wire_rendering ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::backup_replication_transport:
      return surface_kind == ExecutionDetailClosureSurfaceKind::backup_restore ||
             surface_kind == ExecutionDetailClosureSurfaceKind::replication ||
             surface_kind == ExecutionDetailClosureSurfaceKind::cluster_transport ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::system_catalog:
      return surface_kind == ExecutionDetailClosureSurfaceKind::catalog ||
             surface_kind == ExecutionDetailClosureSurfaceKind::security ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
    case ExecutionDetailClosureMatrixKind::conformance_corpus:
      return surface_kind == ExecutionDetailClosureSurfaceKind::conformance ||
             surface_kind == ExecutionDetailClosureSurfaceKind::diagnostic ||
             surface_kind == ExecutionDetailClosureSurfaceKind::metrics ||
             surface_kind == ExecutionDetailClosureSurfaceKind::implementation_trace;
  }
  return false;
}

inline bool ExecutionDetailClosureRowHasSurface(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureRowRecord& row,
    ExecutionDetailClosureSurfaceKind surface_kind,
    bool require_closed) noexcept {
  for (const auto& surface : registry.surfaces) {
    if (!ExecutionDataPacketUuidEquals(surface.matrix_row_uuid,
                                       row.matrix_row_uuid) ||
        surface.surface_kind != surface_kind) {
      continue;
    }
    if (!require_closed ||
        ExecutionDetailClosureSurfaceClosed(surface.surface_status)) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureRowHasCompleteDocumentation(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureRowRecord& row) noexcept {
  for (const auto& surface : registry.surfaces) {
    if (ExecutionDataPacketUuidEquals(surface.matrix_row_uuid,
                                      row.matrix_row_uuid) &&
        surface.surface_kind ==
            ExecutionDetailClosureSurfaceKind::documentation &&
        surface.surface_status ==
            ExecutionDetailClosureSurfaceStatus::complete &&
        !surface.evidence_hash.empty()) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureHasClaimForRow(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureRowRecord& row) noexcept {
  for (const auto& claim : registry.claims) {
    if (ExecutionDataPacketUuidEquals(claim.matrix_row_uuid,
                                      row.matrix_row_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureRowHasAcceptedSupersession(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureRowRecord& row) noexcept {
  for (const auto& supersession : registry.supersessions) {
    if (ExecutionDataPacketUuidEquals(supersession.old_matrix_row_uuid,
                                      row.matrix_row_uuid) &&
        supersession.decision_status ==
            ExecutionDetailClosureSupersessionDecision::accepted) {
      return true;
    }
  }
  return false;
}

inline bool ExecutionDetailClosureSeedSubjectCovered(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureMatrixRecord& matrix,
    std::string_view subject) noexcept {
  for (const auto& row : registry.rows) {
    if (!ExecutionDataPacketUuidEquals(row.matrix_uuid, matrix.matrix_uuid) ||
        row.row_subject != subject) {
      continue;
    }
    if (row.completion_state !=
            ExecutionDetailClosureCompletionState::superseded ||
        ExecutionDetailClosureRowHasAcceptedSupersession(registry, row)) {
      return true;
    }
  }
  return false;
}

inline ExecutionDetailClosureValidationResult
ValidateExecutionDetailClosureMatrix(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureMatrixRecord& matrix,
    std::size_t matrix_index) {
  if (ExecutionDataPacketUuidIsNil(matrix.matrix_uuid)) {
    return {ExecutionDetailClosureStatus::matrix_uuid_required, matrix_index};
  }
  if (!ExecutionDetailClosureMatrixKindIsValid(matrix.matrix_kind)) {
    return {ExecutionDetailClosureStatus::matrix_kind_invalid, matrix_index};
  }
  if (matrix.matrix_name.empty() || matrix.matrix_search_key.empty() ||
      matrix.matrix_search_key !=
          ExecutionDetailClosureMatrixKindSearchKey(matrix.matrix_kind) ||
      matrix.controlling_appendix_set_hash.empty() ||
      matrix.minimum_inventory_hash.empty() ||
      matrix.conformance_manifest_hash.empty() || matrix.matrix_hash.empty()) {
    return {ExecutionDetailClosureStatus::matrix_identity_incomplete,
            matrix_index};
  }
  if (!ExecutionDetailClosureCoverageStatusIsValid(matrix.coverage_status)) {
    return {ExecutionDetailClosureStatus::matrix_identity_incomplete,
            matrix_index};
  }
  if (matrix.coverage_status == ExecutionDetailClosureCoverageStatus::complete &&
      ExecutionDataPacketUuidIsNil(matrix.implementation_trace_uuid)) {
    return {ExecutionDetailClosureStatus::implementation_trace_missing,
            matrix_index};
  }
  if (!ExecutionDataPacketUuidIsNil(matrix.successor_matrix_uuid) &&
      ExecutionDetailClosureFindMatrixByUuid(
          registry, matrix.successor_matrix_uuid) == nullptr) {
    return {ExecutionDetailClosureStatus::matrix_successor_unresolved,
            matrix_index};
  }
  for (std::size_t other_index = matrix_index + 1;
       other_index < registry.matrices.size(); ++other_index) {
    const auto& other = registry.matrices[other_index];
    if (matrix.matrix_search_key == other.matrix_search_key ||
        matrix.matrix_kind == other.matrix_kind ||
        ExecutionDataPacketUuidEquals(matrix.matrix_uuid, other.matrix_uuid)) {
      return {ExecutionDetailClosureStatus::matrix_duplicate, other_index};
    }
  }
  if (matrix.required_seed_subjects.empty()) {
    return {ExecutionDetailClosureStatus::seed_inventory_incomplete,
            matrix_index};
  }
  for (const auto& subject : matrix.required_seed_subjects) {
    if (!ExecutionDetailClosureSeedSubjectCovered(registry, matrix, subject)) {
      return {ExecutionDetailClosureStatus::seed_inventory_incomplete,
              matrix_index};
    }
  }
  return {};
}

inline ExecutionDetailClosureValidationResult ValidateExecutionDetailClosureRow(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureRowRecord& row,
    std::size_t row_index) {
  if (ExecutionDataPacketUuidIsNil(row.matrix_row_uuid)) {
    return {ExecutionDetailClosureStatus::matrix_row_uuid_required, 0,
            row_index};
  }
  const auto* matrix = ExecutionDetailClosureFindMatrixByUuid(
      registry, row.matrix_uuid);
  if (matrix == nullptr) {
    return {ExecutionDetailClosureStatus::matrix_record_missing, 0,
            row_index};
  }
  if (row.row_search_key.empty() || row.row_subject.empty() ||
      row.representation_or_status.empty() ||
      row.controlling_spec_path.empty() || row.controlling_search_key.empty() ||
      row.required_surface_set_hash.empty() ||
      row.completed_surface_set_hash.empty() || row.row_hash.empty()) {
    return {ExecutionDetailClosureStatus::matrix_row_identity_incomplete, 0,
            row_index};
  }
  if (!ExecutionDetailClosureRowScopeIsValid(row.row_scope)) {
    return {ExecutionDetailClosureStatus::row_scope_invalid, 0, row_index};
  }
  if (!ExecutionDetailClosureCompletionStateIsValid(row.completion_state)) {
    return {ExecutionDetailClosureStatus::row_completion_state_invalid, 0,
            row_index};
  }
  if (ExecutionDetailClosureCompletionRequiresDiagnostic(
          row.completion_state) &&
      row.diagnostic_code.empty()) {
    return {ExecutionDetailClosureStatus::
                deferred_or_unsupported_diagnostic_missing,
            0, row_index};
  }
  if (row.diagnostic_code.empty()) {
    return {ExecutionDetailClosureStatus::matrix_row_identity_incomplete, 0,
            row_index};
  }
  if (row.completion_state == ExecutionDetailClosureCompletionState::complete &&
      (row.conformance_case_set_hash.empty() ||
       ExecutionDataPacketUuidIsNil(row.implementation_trace_uuid))) {
    return {
        row.conformance_case_set_hash.empty()
            ? ExecutionDetailClosureStatus::conformance_case_missing
            : ExecutionDetailClosureStatus::implementation_trace_missing,
        0, row_index};
  }
  if (row.completion_state ==
          ExecutionDetailClosureCompletionState::superseded &&
      !ExecutionDetailClosureRowHasAcceptedSupersession(registry, row)) {
    return {ExecutionDetailClosureStatus::supersession_record_missing, 0,
            row_index};
  }
  constexpr ExecutionDetailClosureSurfaceKind all_surfaces[] = {
      ExecutionDetailClosureSurfaceKind::descriptor,
      ExecutionDetailClosureSurfaceKind::domain,
      ExecutionDetailClosureSurfaceKind::literal_bind,
      ExecutionDetailClosureSurfaceKind::cast,
      ExecutionDetailClosureSurfaceKind::operation,
      ExecutionDetailClosureSurfaceKind::aggregate_window,
      ExecutionDetailClosureSurfaceKind::index,
      ExecutionDetailClosureSurfaceKind::statistics,
      ExecutionDetailClosureSurfaceKind::driver_metadata,
      ExecutionDetailClosureSurfaceKind::backup_restore,
      ExecutionDetailClosureSurfaceKind::replication,
      ExecutionDetailClosureSurfaceKind::cluster_transport,
      ExecutionDetailClosureSurfaceKind::storage_encoding,
      ExecutionDetailClosureSurfaceKind::wire_rendering,
      ExecutionDetailClosureSurfaceKind::diagnostic,
      ExecutionDetailClosureSurfaceKind::security,
      ExecutionDetailClosureSurfaceKind::metrics,
      ExecutionDetailClosureSurfaceKind::catalog,
      ExecutionDetailClosureSurfaceKind::conformance,
      ExecutionDetailClosureSurfaceKind::implementation_trace,
      ExecutionDetailClosureSurfaceKind::documentation};
  for (const auto surface_kind : all_surfaces) {
    if (ExecutionDetailClosureSurfaceRequiredForMatrix(matrix->matrix_kind,
                                                       surface_kind) &&
        !ExecutionDetailClosureRowHasSurface(registry, row, surface_kind,
                                             false)) {
      return {ExecutionDetailClosureStatus::surface_record_missing, 0,
              row_index};
    }
  }
  if (row.completion_state == ExecutionDetailClosureCompletionState::complete) {
    for (const auto surface_kind : all_surfaces) {
      if (ExecutionDetailClosureSurfaceRequiredForMatrix(matrix->matrix_kind,
                                                         surface_kind) &&
          !ExecutionDetailClosureRowHasSurface(registry, row, surface_kind,
                                               true)) {
        return {ExecutionDetailClosureStatus::row_surface_coverage_incomplete,
                0, row_index};
      }
    }
    if (!ExecutionDetailClosureHasClaimForRow(registry, row)) {
      return {ExecutionDetailClosureStatus::closure_claim_missing, 0,
              row_index};
    }
  }
  return {};
}

inline ExecutionDetailClosureValidationResult
ValidateExecutionDetailClosureSurface(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureSurfaceRecord& surface,
    std::size_t surface_index) {
  if (ExecutionDataPacketUuidIsNil(surface.surface_uuid)) {
    return {ExecutionDetailClosureStatus::surface_uuid_required, 0, 0,
            surface_index};
  }
  if (ExecutionDetailClosureFindRowByUuid(registry, surface.matrix_row_uuid) ==
      nullptr) {
    return {ExecutionDetailClosureStatus::matrix_row_missing, 0, 0,
            surface_index};
  }
  if (!ExecutionDetailClosureSurfaceKindIsValid(surface.surface_kind)) {
    return {ExecutionDetailClosureStatus::surface_kind_invalid, 0, 0,
            surface_index};
  }
  if (!ExecutionDetailClosureSurfaceStatusIsValid(surface.surface_status)) {
    return {ExecutionDetailClosureStatus::surface_status_invalid, 0, 0,
            surface_index};
  }
  if (surface.owning_spec_path.empty() || surface.owning_search_key.empty() ||
      surface.failure_diagnostic_code.empty() || surface.surface_hash.empty()) {
    return {ExecutionDetailClosureStatus::surface_identity_incomplete, 0, 0,
            surface_index};
  }
  if (surface.surface_status == ExecutionDetailClosureSurfaceStatus::complete &&
      surface.evidence_hash.empty()) {
    return {ExecutionDetailClosureStatus::surface_evidence_missing, 0, 0,
            surface_index};
  }
  return {};
}

inline ExecutionDetailClosureValidationResult
ValidateExecutionDetailClosureClaim(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailClosureClaimRecord& claim,
    std::size_t claim_index) {
  if (ExecutionDataPacketUuidIsNil(claim.closure_claim_uuid)) {
    return {ExecutionDetailClosureStatus::claim_uuid_required, 0, 0, 0,
            claim_index};
  }
  const auto* row =
      ExecutionDetailClosureFindRowByUuid(registry, claim.matrix_row_uuid);
  if (row == nullptr) {
    return {ExecutionDetailClosureStatus::matrix_row_missing, 0, 0, 0,
            claim_index};
  }
  if (!ExecutionDetailClosureClaimScopeIsValid(claim.claim_scope)) {
    return {ExecutionDetailClosureStatus::claim_scope_invalid, 0, 0, 0,
            claim_index};
  }
  if (!ExecutionDetailClosureClaimStatusIsValid(claim.claim_status)) {
    return {ExecutionDetailClosureStatus::claim_status_invalid, 0, 0, 0,
            claim_index};
  }
  if (claim.decision_diagnostic_code.empty() || claim.claim_hash.empty()) {
    return {ExecutionDetailClosureStatus::claim_identity_incomplete, 0, 0, 0,
            claim_index};
  }
  if (claim.claim_status != ExecutionDetailClosureClaimStatus::go) {
    return {};
  }
  if (claim.required_evidence_hash.empty() ||
      claim.provided_evidence_hash.empty() ||
      claim.required_evidence_hash != claim.provided_evidence_hash) {
    return {ExecutionDetailClosureStatus::claim_evidence_incomplete, 0, 0, 0,
            claim_index};
  }
  if (row->completion_state != ExecutionDetailClosureCompletionState::complete) {
    return {ExecutionDetailClosureStatus::row_surface_coverage_incomplete, 0,
            0, 0, claim_index};
  }
  if (ExecutionDetailClosureClaimRequiresConformance(claim.claim_scope) &&
      row->conformance_case_set_hash.empty()) {
    return {ExecutionDetailClosureStatus::conformance_case_missing, 0, 0, 0,
            claim_index};
  }
  if (ExecutionDetailClosureClaimRequiresImplementationTrace(
          claim.claim_scope) &&
      ExecutionDataPacketUuidIsNil(row->implementation_trace_uuid)) {
    return {ExecutionDetailClosureStatus::implementation_trace_missing, 0, 0,
            0, claim_index};
  }
  if (claim.claim_scope == ExecutionDetailClosureClaimScope::native_or_better &&
      (row->representation_or_status.find("native") == std::string::npos ||
       !ExecutionDetailClosureRowHasCompleteDocumentation(registry, *row))) {
    return {ExecutionDetailClosureStatus::native_or_better_unproven, 0, 0, 0,
            claim_index};
  }
  return {};
}

inline ExecutionDetailClosureValidationResult
ValidateExecutionDetailSupersession(
    const ExecutionDetailClosureRegistry& registry,
    const ExecutionDetailSupersessionRecord& supersession,
    std::size_t supersession_index) {
  if (ExecutionDataPacketUuidIsNil(supersession.supersession_uuid)) {
    return {ExecutionDetailClosureStatus::supersession_uuid_required, 0, 0, 0,
            0, supersession_index};
  }
  const auto* old_row = ExecutionDetailClosureFindRowByUuid(
      registry, supersession.old_matrix_row_uuid);
  const auto* new_row = ExecutionDetailClosureFindRowByUuid(
      registry, supersession.new_matrix_row_uuid);
  if (old_row == nullptr || new_row == nullptr ||
      supersession.old_search_key.empty() ||
      supersession.new_search_key.empty() ||
      supersession.supersession_hash.empty() ||
      supersession.old_search_key != old_row->row_search_key ||
      supersession.new_search_key != new_row->row_search_key) {
    return {ExecutionDetailClosureStatus::supersession_identity_incomplete, 0,
            0, 0, 0, supersession_index};
  }
  if (!ExecutionDetailClosureSupersessionReasonIsValid(
          supersession.supersession_reason) ||
      !ExecutionDetailClosureSupersessionDecisionIsValid(
          supersession.decision_status)) {
    return {ExecutionDetailClosureStatus::supersession_policy_invalid, 0, 0, 0,
            0, supersession_index};
  }
  if (old_row->completion_state ==
          ExecutionDetailClosureCompletionState::superseded &&
      supersession.decision_status !=
          ExecutionDetailClosureSupersessionDecision::accepted) {
    return {ExecutionDetailClosureStatus::supersession_not_accepted, 0, 0, 0,
            0, supersession_index};
  }
  return {};
}

inline ExecutionDetailClosureValidationResult
ValidateExecutionDetailClosureRegistry(
    const ExecutionDetailClosureRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {ExecutionDetailClosureStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {ExecutionDetailClosureStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {ExecutionDetailClosureStatus::registry_name_required};
  }
  if (registry.detail_closure_search_key !=
      "EDC-DETAIL-CLOSURE-MATRICES") {
    return {ExecutionDetailClosureStatus::closure_search_key_required};
  }
  if (!registry.descriptor_authoritative) {
    return {ExecutionDetailClosureStatus::descriptor_not_authoritative};
  }
  if (!registry.parser_independent) {
    return {ExecutionDetailClosureStatus::parser_independent_required};
  }
  if (registry.matrices.empty()) {
    return {ExecutionDetailClosureStatus::matrix_records_required};
  }
  if (registry.rows.empty()) {
    return {ExecutionDetailClosureStatus::row_records_required};
  }
  if (registry.surfaces.empty()) {
    return {ExecutionDetailClosureStatus::surface_records_required};
  }
  constexpr ExecutionDetailClosureMatrixKind required_kinds[] = {
      ExecutionDetailClosureMatrixKind::canonical_type_family,
      ExecutionDetailClosureMatrixKind::reference_type_coverage,
      ExecutionDetailClosureMatrixKind::operation_registry,
      ExecutionDetailClosureMatrixKind::cast_registry,
      ExecutionDetailClosureMatrixKind::aggregate_window,
      ExecutionDetailClosureMatrixKind::index_family_compatibility,
      ExecutionDetailClosureMatrixKind::statistics_selectivity,
      ExecutionDetailClosureMatrixKind::driver_metadata,
      ExecutionDetailClosureMatrixKind::backup_replication_transport,
      ExecutionDetailClosureMatrixKind::system_catalog,
      ExecutionDetailClosureMatrixKind::conformance_corpus};
  for (const auto kind : required_kinds) {
    if (!ExecutionDetailClosureHasMatrixKind(registry, kind)) {
      return {ExecutionDetailClosureStatus::matrix_record_missing};
    }
  }
  for (std::size_t matrix_index = 0; matrix_index < registry.matrices.size();
       ++matrix_index) {
    const auto matrix_result = ValidateExecutionDetailClosureMatrix(
        registry, registry.matrices[matrix_index], matrix_index);
    if (!matrix_result.ok()) {
      return matrix_result;
    }
  }
  for (std::size_t supersession_index = 0;
       supersession_index < registry.supersessions.size();
       ++supersession_index) {
    const auto supersession_result = ValidateExecutionDetailSupersession(
        registry, registry.supersessions[supersession_index],
        supersession_index);
    if (!supersession_result.ok()) {
      return supersession_result;
    }
  }
  for (std::size_t surface_index = 0; surface_index < registry.surfaces.size();
       ++surface_index) {
    const auto surface_result = ValidateExecutionDetailClosureSurface(
        registry, registry.surfaces[surface_index], surface_index);
    if (!surface_result.ok()) {
      return surface_result;
    }
  }
  for (std::size_t row_index = 0; row_index < registry.rows.size();
       ++row_index) {
    const auto row_result =
        ValidateExecutionDetailClosureRow(registry, registry.rows[row_index],
                                          row_index);
    if (!row_result.ok()) {
      return row_result;
    }
  }
  for (std::size_t claim_index = 0; claim_index < registry.claims.size();
       ++claim_index) {
    const auto claim_result = ValidateExecutionDetailClosureClaim(
        registry, registry.claims[claim_index], claim_index);
    if (!claim_result.ok()) {
      return claim_result;
    }
  }
  if (!registry.diagnostics_root_declared) {
    return {ExecutionDetailClosureStatus::diagnostic_vector_missing};
  }
  constexpr std::string_view required_diagnostics[] = {
      "EDC.MATRIX_RECORD_MISSING",
      "EDC.MATRIX_ROW_MISSING",
      "EDC.MATRIX_ROW_IDENTITY_INCOMPLETE",
      "EDC.SURFACE_RECORD_MISSING",
      "EDC.ROW_SURFACE_COVERAGE_INCOMPLETE",
      "EDC.SEED_INVENTORY_INCOMPLETE",
      "EDC.SUPERSESSION_RECORD_MISSING",
      "EDC.CLOSURE_CLAIM_MISSING",
      "EDC.CLAIM_EVIDENCE_INCOMPLETE",
      "EDC.CONFORMANCE_CASE_MISSING",
      "EDC.IMPLEMENTATION_TRACE_MISSING",
      "EDC.DEFERRED_OR_UNSUPPORTED_DIAGNOSTIC_MISSING",
      "EDC.NATIVE_OR_BETTER_UNPROVEN"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!ExecutionDetailClosureHasDiagnosticCode(registry,
                                                 diagnostic_code)) {
      return {ExecutionDetailClosureStatus::diagnostic_vector_missing};
    }
  }
  if (!registry.local_metrics_root_declared) {
    return {ExecutionDetailClosureStatus::local_metrics_root_required};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.execution_detail_closure.matrix_count",
      "sys.metrics.execution_detail_closure.row_count",
      "sys.metrics.execution_detail_closure.surface_count",
      "sys.metrics.execution_detail_closure.matrix_record_missing_total",
      "sys.metrics.execution_detail_closure.matrix_row_missing_total",
      "sys.metrics.execution_detail_closure.row_identity_incomplete_total",
      "sys.metrics.execution_detail_closure.surface_missing_total",
      "sys.metrics.execution_detail_closure.surface_incomplete_total",
      "sys.metrics.execution_detail_closure.seed_inventory_incomplete_total",
      "sys.metrics.execution_detail_closure.supersession_missing_total",
      "sys.metrics.execution_detail_closure.claim_evidence_incomplete_total",
      "sys.metrics.execution_detail_closure.conformance_case_missing_total",
      "sys.metrics.execution_detail_closure.implementation_trace_missing_total",
      "sys.metrics.execution_detail_closure.native_or_better_unproven_total"};
  for (std::string_view metric : required_metrics) {
    if (!ExecutionDetailClosureHasMetric(registry, metric)) {
      return {ExecutionDetailClosureStatus::local_metric_missing};
    }
  }
  return {};
}

// SEARCH_KEY: RDC-REMAINING-DETAIL-CLOSURE-CONTROLS
// SEARCH_KEY: RDC-001
// SEARCH_KEY: RDC-010
// SEARCH_KEY: RDC-011
// SEARCH_KEY: RDC-016
// SEARCH_KEY: SCS-SYSTEM-CATALOG-SCHEMA
// SEARCH_KEY: TSP-TYPE-SECURITY-PRIVILEGE-MATRIX
// SEARCH_KEY: DER-DIAGNOSTIC-ERROR-CODE-REGISTRY
// SEARCH_KEY: RRV-RESOURCE-REGISTRY-VERSIONING
// SEARCH_KEY: PSG-PARSER-SBLR-GRAMMAR-EXPANSION
// SEARCH_KEY: DSM-REFERENCE-SPECIFIC-TYPE-MATRICES
// SEARCH_KEY: CEE-CANONICAL-ENCODING-EXAMPLES
// SEARCH_KEY: CMM-COMPATIBILITY-MODE-MATRIX
// SEARCH_KEY: CTM-CONFORMANCE-TEST-MANIFEST-FORMAT
// SEARCH_KEY: DEP-DOCUMENTATION-EXAMPLES-POLICY
// SEARCH_KEY: SCT-SYSTEM-CATALOG-TABLE-DEFINITIONS
// SEARCH_KEY: PSF-PARSER-SBLR-FORMAL-GRAMMAR
// SEARCH_KEY: NEE-NORMATIVE-ENCODING-EXAMPLES
// SEARCH_KEY: CMI-CONFORMANCE-MANIFEST-INVENTORY
// SEARCH_KEY: DVP-REFERENCE-VERSION-PROFILE-CLOSURE
// SEARCH_KEY: CGC-COMMERCIAL-GRADE-COMPLETION-GATES
enum class RemainingDetailClosureControlKind : std::uint8_t {
  system_catalog_schema = 0,
  type_security_privilege = 1,
  diagnostic_registry = 2,
  resource_registry_versioning = 3,
  parser_sblr_grammar_expansion = 4,
  reference_specific_type_matrices = 5,
  canonical_encoding_examples = 6,
  compatibility_mode_matrix = 7,
  conformance_test_manifest_format = 8,
  documentation_examples_policy = 9,
  system_catalog_table_definitions = 10,
  parser_sblr_formal_grammar = 11,
  normative_encoding_examples = 12,
  conformance_manifest_inventory = 13,
  reference_version_profile_closure = 14,
  commercial_grade_completion_gates = 15
};

constexpr bool RemainingDetailClosureControlKindIsValid(
    RemainingDetailClosureControlKind kind) noexcept {
  switch (kind) {
    case RemainingDetailClosureControlKind::system_catalog_schema:
    case RemainingDetailClosureControlKind::type_security_privilege:
    case RemainingDetailClosureControlKind::diagnostic_registry:
    case RemainingDetailClosureControlKind::resource_registry_versioning:
    case RemainingDetailClosureControlKind::parser_sblr_grammar_expansion:
    case RemainingDetailClosureControlKind::reference_specific_type_matrices:
    case RemainingDetailClosureControlKind::canonical_encoding_examples:
    case RemainingDetailClosureControlKind::compatibility_mode_matrix:
    case RemainingDetailClosureControlKind::conformance_test_manifest_format:
    case RemainingDetailClosureControlKind::documentation_examples_policy:
    case RemainingDetailClosureControlKind::system_catalog_table_definitions:
    case RemainingDetailClosureControlKind::parser_sblr_formal_grammar:
    case RemainingDetailClosureControlKind::normative_encoding_examples:
    case RemainingDetailClosureControlKind::conformance_manifest_inventory:
    case RemainingDetailClosureControlKind::reference_version_profile_closure:
    case RemainingDetailClosureControlKind::commercial_grade_completion_gates:
      return true;
  }
  return false;
}

constexpr std::string_view RemainingDetailClosureControlSearchKey(
    RemainingDetailClosureControlKind kind) noexcept {
  switch (kind) {
    case RemainingDetailClosureControlKind::system_catalog_schema:
      return "SCS-SYSTEM-CATALOG-SCHEMA";
    case RemainingDetailClosureControlKind::type_security_privilege:
      return "TSP-TYPE-SECURITY-PRIVILEGE-MATRIX";
    case RemainingDetailClosureControlKind::diagnostic_registry:
      return "DER-DIAGNOSTIC-ERROR-CODE-REGISTRY";
    case RemainingDetailClosureControlKind::resource_registry_versioning:
      return "RRV-RESOURCE-REGISTRY-VERSIONING";
    case RemainingDetailClosureControlKind::parser_sblr_grammar_expansion:
      return "PSG-PARSER-SBLR-GRAMMAR-EXPANSION";
    case RemainingDetailClosureControlKind::reference_specific_type_matrices:
      return "DSM-REFERENCE-SPECIFIC-TYPE-MATRICES";
    case RemainingDetailClosureControlKind::canonical_encoding_examples:
      return "CEE-CANONICAL-ENCODING-EXAMPLES";
    case RemainingDetailClosureControlKind::compatibility_mode_matrix:
      return "CMM-COMPATIBILITY-MODE-MATRIX";
    case RemainingDetailClosureControlKind::conformance_test_manifest_format:
      return "CTM-CONFORMANCE-TEST-MANIFEST-FORMAT";
    case RemainingDetailClosureControlKind::documentation_examples_policy:
      return "DEP-DOCUMENTATION-EXAMPLES-POLICY";
    case RemainingDetailClosureControlKind::system_catalog_table_definitions:
      return "SCT-SYSTEM-CATALOG-TABLE-DEFINITIONS";
    case RemainingDetailClosureControlKind::parser_sblr_formal_grammar:
      return "PSF-PARSER-SBLR-FORMAL-GRAMMAR";
    case RemainingDetailClosureControlKind::normative_encoding_examples:
      return "NEE-NORMATIVE-ENCODING-EXAMPLES";
    case RemainingDetailClosureControlKind::conformance_manifest_inventory:
      return "CMI-CONFORMANCE-MANIFEST-INVENTORY";
    case RemainingDetailClosureControlKind::reference_version_profile_closure:
      return "DVP-REFERENCE-VERSION-PROFILE-CLOSURE";
    case RemainingDetailClosureControlKind::commercial_grade_completion_gates:
      return "CGC-COMMERCIAL-GRADE-COMPLETION-GATES";
  }
  return "";
}

enum class RemainingDetailClosureControlStatus : std::uint8_t {
  complete = 0,
  partial = 1,
  deferred = 2,
  unsupported_by_policy = 3,
  implementation_pending = 4
};

constexpr bool RemainingDetailClosureControlStatusIsValid(
    RemainingDetailClosureControlStatus status) noexcept {
  switch (status) {
    case RemainingDetailClosureControlStatus::complete:
    case RemainingDetailClosureControlStatus::partial:
    case RemainingDetailClosureControlStatus::deferred:
    case RemainingDetailClosureControlStatus::unsupported_by_policy:
    case RemainingDetailClosureControlStatus::implementation_pending:
      return true;
  }
  return false;
}

enum class RemainingDetailClosureSurfaceKind : std::uint8_t {
  catalog_schema = 0,
  privilege_policy = 1,
  diagnostic_registry = 2,
  resource_versioning = 3,
  parser_sblr_lowering = 4,
  reference_matrix_file = 5,
  canonical_encoding_example = 6,
  compatibility_mode = 7,
  conformance_manifest = 8,
  documentation_policy = 9,
  system_catalog_table = 10,
  formal_grammar = 11,
  normative_encoding_example = 12,
  manifest_inventory = 13,
  reference_version_profile = 14,
  commercial_gate = 15,
  security = 16,
  diagnostic = 17,
  metrics = 18,
  implementation_trace = 19
};

constexpr bool RemainingDetailClosureSurfaceKindIsValid(
    RemainingDetailClosureSurfaceKind kind) noexcept {
  switch (kind) {
    case RemainingDetailClosureSurfaceKind::catalog_schema:
    case RemainingDetailClosureSurfaceKind::privilege_policy:
    case RemainingDetailClosureSurfaceKind::diagnostic_registry:
    case RemainingDetailClosureSurfaceKind::resource_versioning:
    case RemainingDetailClosureSurfaceKind::parser_sblr_lowering:
    case RemainingDetailClosureSurfaceKind::reference_matrix_file:
    case RemainingDetailClosureSurfaceKind::canonical_encoding_example:
    case RemainingDetailClosureSurfaceKind::compatibility_mode:
    case RemainingDetailClosureSurfaceKind::conformance_manifest:
    case RemainingDetailClosureSurfaceKind::documentation_policy:
    case RemainingDetailClosureSurfaceKind::system_catalog_table:
    case RemainingDetailClosureSurfaceKind::formal_grammar:
    case RemainingDetailClosureSurfaceKind::normative_encoding_example:
    case RemainingDetailClosureSurfaceKind::manifest_inventory:
    case RemainingDetailClosureSurfaceKind::reference_version_profile:
    case RemainingDetailClosureSurfaceKind::commercial_gate:
    case RemainingDetailClosureSurfaceKind::security:
    case RemainingDetailClosureSurfaceKind::diagnostic:
    case RemainingDetailClosureSurfaceKind::metrics:
    case RemainingDetailClosureSurfaceKind::implementation_trace:
      return true;
  }
  return false;
}

enum class RemainingDetailClosureSurfaceStatus : std::uint8_t {
  complete = 0,
  partial = 1,
  deferred = 2,
  unsupported_by_policy = 3,
  implementation_pending = 4
};

constexpr bool RemainingDetailClosureSurfaceStatusIsValid(
    RemainingDetailClosureSurfaceStatus status) noexcept {
  switch (status) {
    case RemainingDetailClosureSurfaceStatus::complete:
    case RemainingDetailClosureSurfaceStatus::partial:
    case RemainingDetailClosureSurfaceStatus::deferred:
    case RemainingDetailClosureSurfaceStatus::unsupported_by_policy:
    case RemainingDetailClosureSurfaceStatus::implementation_pending:
      return true;
  }
  return false;
}

enum class RemainingDetailClosureGateStatus : std::uint8_t {
  go = 0,
  no_go = 1,
  deferred = 2,
  blocked = 3
};

constexpr bool RemainingDetailClosureGateStatusIsValid(
    RemainingDetailClosureGateStatus status) noexcept {
  switch (status) {
    case RemainingDetailClosureGateStatus::go:
    case RemainingDetailClosureGateStatus::no_go:
    case RemainingDetailClosureGateStatus::deferred:
    case RemainingDetailClosureGateStatus::blocked:
      return true;
  }
  return false;
}

struct RemainingDetailClosureControlRecord {
  Uuid control_uuid{};
  std::string rdc_id;
  RemainingDetailClosureControlKind control_kind =
      RemainingDetailClosureControlKind::system_catalog_schema;
  std::string control_search_key;
  std::string controlling_spec_path;
  std::string controlling_spec_hash;
  std::string owner_subsystem;
  RemainingDetailClosureControlStatus status =
      RemainingDetailClosureControlStatus::implementation_pending;
  std::vector<std::string> required_gate_keys;
  Uuid implementation_trace_uuid{};
  std::string conformance_manifest_hash;
  std::string diagnostic_code;
  std::string control_hash;
  bool descriptor_authority_preserved = true;
  bool parser_boundary_preserved = true;
  bool reference_sql_not_engine_authority = true;
  bool security_policy_enforced = true;
  bool resource_invalidation_declared = true;
  bool silent_fallback_forbidden = true;
  bool examples_non_authoritative = true;
};

struct RemainingDetailClosureSurfaceRecord {
  Uuid surface_uuid{};
  Uuid control_uuid{};
  RemainingDetailClosureSurfaceKind surface_kind =
      RemainingDetailClosureSurfaceKind::catalog_schema;
  RemainingDetailClosureSurfaceStatus surface_status =
      RemainingDetailClosureSurfaceStatus::implementation_pending;
  std::string owning_search_key;
  std::string evidence_hash;
  std::string failure_diagnostic_code;
  std::string surface_hash;
};

struct RemainingDetailClosureGateRecord {
  Uuid gate_uuid{};
  Uuid control_uuid{};
  std::string gate_key;
  RemainingDetailClosureGateStatus gate_status =
      RemainingDetailClosureGateStatus::no_go;
  std::string required_evidence_hash;
  std::string provided_evidence_hash;
  Uuid implementation_trace_uuid{};
  std::string diagnostic_code;
  std::string gate_hash;
};

struct RemainingDetailClosureRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string registry_name;
  std::string root_search_key;
  std::vector<RemainingDetailClosureControlRecord> controls;
  std::vector<RemainingDetailClosureSurfaceRecord> surfaces;
  std::vector<RemainingDetailClosureGateRecord> gates;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
  bool diagnostics_root_declared = true;
  bool local_metrics_root_declared = true;
};

enum class RemainingDetailClosureStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  root_search_key_required = 4,
  descriptor_not_authoritative = 5,
  parser_independent_required = 6,
  control_records_required = 7,
  surface_records_required = 8,
  gate_records_required = 9,
  control_record_missing = 10,
  control_uuid_required = 11,
  control_identity_incomplete = 12,
  control_kind_invalid = 13,
  control_status_invalid = 14,
  control_duplicate = 15,
  implementation_trace_missing = 16,
  conformance_manifest_missing = 17,
  parser_authority_violation = 18,
  security_policy_missing = 19,
  resource_invalidation_missing = 20,
  silent_fallback_allowed = 21,
  example_authority_violation = 22,
  surface_record_missing = 23,
  surface_uuid_required = 24,
  surface_identity_incomplete = 25,
  surface_kind_invalid = 26,
  surface_status_invalid = 27,
  surface_evidence_missing = 28,
  surface_incomplete = 29,
  gate_record_missing = 30,
  gate_uuid_required = 31,
  gate_identity_incomplete = 32,
  gate_status_invalid = 33,
  gate_evidence_incomplete = 34,
  diagnostic_vector_missing = 35,
  local_metrics_root_required = 36,
  local_metric_missing = 37
};

constexpr std::string_view RemainingDetailClosureStatusName(
    RemainingDetailClosureStatus status) noexcept {
  switch (status) {
    case RemainingDetailClosureStatus::ok:
      return "ok";
    case RemainingDetailClosureStatus::registry_uuid_required:
      return "registry_uuid_required";
    case RemainingDetailClosureStatus::registry_epoch_required:
      return "registry_epoch_required";
    case RemainingDetailClosureStatus::registry_name_required:
      return "registry_name_required";
    case RemainingDetailClosureStatus::root_search_key_required:
      return "root_search_key_required";
    case RemainingDetailClosureStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case RemainingDetailClosureStatus::parser_independent_required:
      return "parser_independent_required";
    case RemainingDetailClosureStatus::control_records_required:
      return "control_records_required";
    case RemainingDetailClosureStatus::surface_records_required:
      return "surface_records_required";
    case RemainingDetailClosureStatus::gate_records_required:
      return "gate_records_required";
    case RemainingDetailClosureStatus::control_record_missing:
      return "control_record_missing";
    case RemainingDetailClosureStatus::control_uuid_required:
      return "control_uuid_required";
    case RemainingDetailClosureStatus::control_identity_incomplete:
      return "control_identity_incomplete";
    case RemainingDetailClosureStatus::control_kind_invalid:
      return "control_kind_invalid";
    case RemainingDetailClosureStatus::control_status_invalid:
      return "control_status_invalid";
    case RemainingDetailClosureStatus::control_duplicate:
      return "control_duplicate";
    case RemainingDetailClosureStatus::implementation_trace_missing:
      return "implementation_trace_missing";
    case RemainingDetailClosureStatus::conformance_manifest_missing:
      return "conformance_manifest_missing";
    case RemainingDetailClosureStatus::parser_authority_violation:
      return "parser_authority_violation";
    case RemainingDetailClosureStatus::security_policy_missing:
      return "security_policy_missing";
    case RemainingDetailClosureStatus::resource_invalidation_missing:
      return "resource_invalidation_missing";
    case RemainingDetailClosureStatus::silent_fallback_allowed:
      return "silent_fallback_allowed";
    case RemainingDetailClosureStatus::example_authority_violation:
      return "example_authority_violation";
    case RemainingDetailClosureStatus::surface_record_missing:
      return "surface_record_missing";
    case RemainingDetailClosureStatus::surface_uuid_required:
      return "surface_uuid_required";
    case RemainingDetailClosureStatus::surface_identity_incomplete:
      return "surface_identity_incomplete";
    case RemainingDetailClosureStatus::surface_kind_invalid:
      return "surface_kind_invalid";
    case RemainingDetailClosureStatus::surface_status_invalid:
      return "surface_status_invalid";
    case RemainingDetailClosureStatus::surface_evidence_missing:
      return "surface_evidence_missing";
    case RemainingDetailClosureStatus::surface_incomplete:
      return "surface_incomplete";
    case RemainingDetailClosureStatus::gate_record_missing:
      return "gate_record_missing";
    case RemainingDetailClosureStatus::gate_uuid_required:
      return "gate_uuid_required";
    case RemainingDetailClosureStatus::gate_identity_incomplete:
      return "gate_identity_incomplete";
    case RemainingDetailClosureStatus::gate_status_invalid:
      return "gate_status_invalid";
    case RemainingDetailClosureStatus::gate_evidence_incomplete:
      return "gate_evidence_incomplete";
    case RemainingDetailClosureStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case RemainingDetailClosureStatus::local_metrics_root_required:
      return "local_metrics_root_required";
    case RemainingDetailClosureStatus::local_metric_missing:
      return "local_metric_missing";
  }
  return "unknown_status";
}

struct RemainingDetailClosureValidationResult {
  RemainingDetailClosureStatus status = RemainingDetailClosureStatus::ok;
  std::size_t control_index = 0;
  std::size_t surface_index = 0;
  std::size_t gate_index = 0;

  bool ok() const noexcept {
    return status == RemainingDetailClosureStatus::ok;
  }
};

inline const RemainingDetailClosureControlRecord*
RemainingDetailClosureFindControlByUuid(
    const RemainingDetailClosureRegistry& registry,
    const Uuid& control_uuid) noexcept {
  for (const auto& control : registry.controls) {
    if (ExecutionDataPacketUuidEquals(control.control_uuid, control_uuid)) {
      return &control;
    }
  }
  return nullptr;
}

inline bool RemainingDetailClosureHasControlKind(
    const RemainingDetailClosureRegistry& registry,
    RemainingDetailClosureControlKind kind) noexcept {
  for (const auto& control : registry.controls) {
    if (control.control_kind == kind) {
      return true;
    }
  }
  return false;
}

constexpr RemainingDetailClosureSurfaceKind
RemainingDetailClosurePrimarySurfaceKind(
    RemainingDetailClosureControlKind kind) noexcept {
  switch (kind) {
    case RemainingDetailClosureControlKind::system_catalog_schema:
      return RemainingDetailClosureSurfaceKind::catalog_schema;
    case RemainingDetailClosureControlKind::type_security_privilege:
      return RemainingDetailClosureSurfaceKind::privilege_policy;
    case RemainingDetailClosureControlKind::diagnostic_registry:
      return RemainingDetailClosureSurfaceKind::diagnostic_registry;
    case RemainingDetailClosureControlKind::resource_registry_versioning:
      return RemainingDetailClosureSurfaceKind::resource_versioning;
    case RemainingDetailClosureControlKind::parser_sblr_grammar_expansion:
      return RemainingDetailClosureSurfaceKind::parser_sblr_lowering;
    case RemainingDetailClosureControlKind::reference_specific_type_matrices:
      return RemainingDetailClosureSurfaceKind::reference_matrix_file;
    case RemainingDetailClosureControlKind::canonical_encoding_examples:
      return RemainingDetailClosureSurfaceKind::canonical_encoding_example;
    case RemainingDetailClosureControlKind::compatibility_mode_matrix:
      return RemainingDetailClosureSurfaceKind::compatibility_mode;
    case RemainingDetailClosureControlKind::conformance_test_manifest_format:
      return RemainingDetailClosureSurfaceKind::conformance_manifest;
    case RemainingDetailClosureControlKind::documentation_examples_policy:
      return RemainingDetailClosureSurfaceKind::documentation_policy;
    case RemainingDetailClosureControlKind::system_catalog_table_definitions:
      return RemainingDetailClosureSurfaceKind::system_catalog_table;
    case RemainingDetailClosureControlKind::parser_sblr_formal_grammar:
      return RemainingDetailClosureSurfaceKind::formal_grammar;
    case RemainingDetailClosureControlKind::normative_encoding_examples:
      return RemainingDetailClosureSurfaceKind::normative_encoding_example;
    case RemainingDetailClosureControlKind::conformance_manifest_inventory:
      return RemainingDetailClosureSurfaceKind::manifest_inventory;
    case RemainingDetailClosureControlKind::reference_version_profile_closure:
      return RemainingDetailClosureSurfaceKind::reference_version_profile;
    case RemainingDetailClosureControlKind::commercial_grade_completion_gates:
      return RemainingDetailClosureSurfaceKind::commercial_gate;
  }
  return RemainingDetailClosureSurfaceKind::diagnostic;
}

constexpr bool RemainingDetailClosureSurfaceRequiredForControl(
    RemainingDetailClosureControlKind control_kind,
    RemainingDetailClosureSurfaceKind surface_kind) noexcept {
  if (surface_kind ==
      RemainingDetailClosurePrimarySurfaceKind(control_kind)) {
    return true;
  }
  if (surface_kind == RemainingDetailClosureSurfaceKind::diagnostic ||
      surface_kind == RemainingDetailClosureSurfaceKind::metrics ||
      surface_kind == RemainingDetailClosureSurfaceKind::implementation_trace) {
    return true;
  }
  if (control_kind ==
          RemainingDetailClosureControlKind::type_security_privilege &&
      surface_kind == RemainingDetailClosureSurfaceKind::security) {
    return true;
  }
  if (control_kind ==
          RemainingDetailClosureControlKind::diagnostic_registry &&
      surface_kind == RemainingDetailClosureSurfaceKind::security) {
    return true;
  }
  return false;
}

inline bool RemainingDetailClosureControlHasSurface(
    const RemainingDetailClosureRegistry& registry,
    const RemainingDetailClosureControlRecord& control,
    RemainingDetailClosureSurfaceKind surface_kind,
    bool require_complete) noexcept {
  for (const auto& surface : registry.surfaces) {
    if (!ExecutionDataPacketUuidEquals(surface.control_uuid,
                                       control.control_uuid) ||
        surface.surface_kind != surface_kind) {
      continue;
    }
    if (!require_complete ||
        surface.surface_status == RemainingDetailClosureSurfaceStatus::complete) {
      return true;
    }
  }
  return false;
}

inline bool RemainingDetailClosureControlHasGate(
    const RemainingDetailClosureRegistry& registry,
    const RemainingDetailClosureControlRecord& control,
    std::string_view gate_key,
    bool require_go) noexcept {
  for (const auto& gate : registry.gates) {
    if (!ExecutionDataPacketUuidEquals(gate.control_uuid,
                                       control.control_uuid) ||
        gate.gate_key != gate_key) {
      continue;
    }
    if (!require_go || gate.gate_status == RemainingDetailClosureGateStatus::go) {
      return true;
    }
  }
  return false;
}

inline bool RemainingDetailClosureHasDiagnosticCode(
    const RemainingDetailClosureRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

inline bool RemainingDetailClosureHasMetric(
    const RemainingDetailClosureRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline RemainingDetailClosureValidationResult
ValidateRemainingDetailClosureControl(
    const RemainingDetailClosureRegistry& registry,
    const RemainingDetailClosureControlRecord& control,
    std::size_t control_index) {
  if (ExecutionDataPacketUuidIsNil(control.control_uuid)) {
    return {RemainingDetailClosureStatus::control_uuid_required,
            control_index};
  }
  if (!RemainingDetailClosureControlKindIsValid(control.control_kind)) {
    return {RemainingDetailClosureStatus::control_kind_invalid,
            control_index};
  }
  if (control.rdc_id.empty() || control.control_search_key.empty() ||
      control.control_search_key !=
          RemainingDetailClosureControlSearchKey(control.control_kind) ||
      control.controlling_spec_path.empty() ||
      control.controlling_spec_hash.empty() ||
      control.owner_subsystem.empty() || control.diagnostic_code.empty() ||
      control.control_hash.empty() || control.required_gate_keys.empty()) {
    return {RemainingDetailClosureStatus::control_identity_incomplete,
            control_index};
  }
  if (!RemainingDetailClosureControlStatusIsValid(control.status)) {
    return {RemainingDetailClosureStatus::control_status_invalid,
            control_index};
  }
  if (control.status == RemainingDetailClosureControlStatus::complete &&
      ExecutionDataPacketUuidIsNil(control.implementation_trace_uuid)) {
    return {RemainingDetailClosureStatus::implementation_trace_missing,
            control_index};
  }
  if (control.status == RemainingDetailClosureControlStatus::complete &&
      control.conformance_manifest_hash.empty()) {
    return {RemainingDetailClosureStatus::conformance_manifest_missing,
            control_index};
  }
  if (!control.descriptor_authority_preserved ||
      !control.parser_boundary_preserved ||
      !control.reference_sql_not_engine_authority) {
    return {RemainingDetailClosureStatus::parser_authority_violation,
            control_index};
  }
  if (!control.security_policy_enforced) {
    return {RemainingDetailClosureStatus::security_policy_missing,
            control_index};
  }
  if (!control.resource_invalidation_declared) {
    return {RemainingDetailClosureStatus::resource_invalidation_missing,
            control_index};
  }
  if (!control.silent_fallback_forbidden) {
    return {RemainingDetailClosureStatus::silent_fallback_allowed,
            control_index};
  }
  if (!control.examples_non_authoritative) {
    return {RemainingDetailClosureStatus::example_authority_violation,
            control_index};
  }
  for (std::size_t other_index = control_index + 1;
       other_index < registry.controls.size(); ++other_index) {
    const auto& other = registry.controls[other_index];
    if (control.control_kind == other.control_kind ||
        control.rdc_id == other.rdc_id ||
        ExecutionDataPacketUuidEquals(control.control_uuid,
                                      other.control_uuid)) {
      return {RemainingDetailClosureStatus::control_duplicate, other_index};
    }
  }
  constexpr RemainingDetailClosureSurfaceKind required_surfaces[] = {
      RemainingDetailClosureSurfaceKind::catalog_schema,
      RemainingDetailClosureSurfaceKind::privilege_policy,
      RemainingDetailClosureSurfaceKind::diagnostic_registry,
      RemainingDetailClosureSurfaceKind::resource_versioning,
      RemainingDetailClosureSurfaceKind::parser_sblr_lowering,
      RemainingDetailClosureSurfaceKind::reference_matrix_file,
      RemainingDetailClosureSurfaceKind::canonical_encoding_example,
      RemainingDetailClosureSurfaceKind::compatibility_mode,
      RemainingDetailClosureSurfaceKind::conformance_manifest,
      RemainingDetailClosureSurfaceKind::documentation_policy,
      RemainingDetailClosureSurfaceKind::system_catalog_table,
      RemainingDetailClosureSurfaceKind::formal_grammar,
      RemainingDetailClosureSurfaceKind::normative_encoding_example,
      RemainingDetailClosureSurfaceKind::manifest_inventory,
      RemainingDetailClosureSurfaceKind::reference_version_profile,
      RemainingDetailClosureSurfaceKind::commercial_gate,
      RemainingDetailClosureSurfaceKind::security,
      RemainingDetailClosureSurfaceKind::diagnostic,
      RemainingDetailClosureSurfaceKind::metrics,
      RemainingDetailClosureSurfaceKind::implementation_trace};
  for (const auto surface_kind : required_surfaces) {
    if (RemainingDetailClosureSurfaceRequiredForControl(control.control_kind,
                                                       surface_kind) &&
        !RemainingDetailClosureControlHasSurface(registry, control,
                                                 surface_kind, false)) {
      return {RemainingDetailClosureStatus::surface_record_missing,
              control_index};
    }
  }
  if (control.status == RemainingDetailClosureControlStatus::complete) {
    for (const auto surface_kind : required_surfaces) {
      if (RemainingDetailClosureSurfaceRequiredForControl(control.control_kind,
                                                         surface_kind) &&
          !RemainingDetailClosureControlHasSurface(registry, control,
                                                   surface_kind, true)) {
        return {RemainingDetailClosureStatus::surface_incomplete,
                control_index};
      }
    }
    for (const auto& gate_key : control.required_gate_keys) {
      if (!RemainingDetailClosureControlHasGate(registry, control, gate_key,
                                                true)) {
        return {RemainingDetailClosureStatus::gate_record_missing,
                control_index};
      }
    }
  }
  return {};
}

inline RemainingDetailClosureValidationResult
ValidateRemainingDetailClosureSurface(
    const RemainingDetailClosureRegistry& registry,
    const RemainingDetailClosureSurfaceRecord& surface,
    std::size_t surface_index) {
  if (ExecutionDataPacketUuidIsNil(surface.surface_uuid)) {
    return {RemainingDetailClosureStatus::surface_uuid_required, 0,
            surface_index};
  }
  if (RemainingDetailClosureFindControlByUuid(registry,
                                              surface.control_uuid) == nullptr) {
    return {RemainingDetailClosureStatus::control_record_missing, 0,
            surface_index};
  }
  if (!RemainingDetailClosureSurfaceKindIsValid(surface.surface_kind)) {
    return {RemainingDetailClosureStatus::surface_kind_invalid, 0,
            surface_index};
  }
  if (!RemainingDetailClosureSurfaceStatusIsValid(surface.surface_status)) {
    return {RemainingDetailClosureStatus::surface_status_invalid, 0,
            surface_index};
  }
  if (surface.owning_search_key.empty() ||
      surface.failure_diagnostic_code.empty() || surface.surface_hash.empty()) {
    return {RemainingDetailClosureStatus::surface_identity_incomplete, 0,
            surface_index};
  }
  if (surface.surface_status == RemainingDetailClosureSurfaceStatus::complete &&
      surface.evidence_hash.empty()) {
    return {RemainingDetailClosureStatus::surface_evidence_missing, 0,
            surface_index};
  }
  return {};
}

inline RemainingDetailClosureValidationResult
ValidateRemainingDetailClosureGate(
    const RemainingDetailClosureRegistry& registry,
    const RemainingDetailClosureGateRecord& gate,
    std::size_t gate_index) {
  if (ExecutionDataPacketUuidIsNil(gate.gate_uuid)) {
    return {RemainingDetailClosureStatus::gate_uuid_required, 0, 0,
            gate_index};
  }
  const auto* control =
      RemainingDetailClosureFindControlByUuid(registry, gate.control_uuid);
  if (control == nullptr) {
    return {RemainingDetailClosureStatus::control_record_missing, 0, 0,
            gate_index};
  }
  if (gate.gate_key.empty() || gate.diagnostic_code.empty() ||
      gate.gate_hash.empty()) {
    return {RemainingDetailClosureStatus::gate_identity_incomplete, 0, 0,
            gate_index};
  }
  if (!RemainingDetailClosureGateStatusIsValid(gate.gate_status)) {
    return {RemainingDetailClosureStatus::gate_status_invalid, 0, 0,
            gate_index};
  }
  if (gate.gate_status == RemainingDetailClosureGateStatus::go &&
      (gate.required_evidence_hash.empty() ||
       gate.provided_evidence_hash.empty() ||
       gate.required_evidence_hash != gate.provided_evidence_hash ||
       ExecutionDataPacketUuidIsNil(gate.implementation_trace_uuid))) {
    return {RemainingDetailClosureStatus::gate_evidence_incomplete, 0, 0,
            gate_index};
  }
  return {};
}

inline RemainingDetailClosureValidationResult
ValidateRemainingDetailClosureRegistry(
    const RemainingDetailClosureRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {RemainingDetailClosureStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {RemainingDetailClosureStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {RemainingDetailClosureStatus::registry_name_required};
  }
  if (registry.root_search_key !=
      "RDC-REMAINING-DETAIL-CLOSURE-CONTROLS") {
    return {RemainingDetailClosureStatus::root_search_key_required};
  }
  if (!registry.descriptor_authoritative) {
    return {RemainingDetailClosureStatus::descriptor_not_authoritative};
  }
  if (!registry.parser_independent) {
    return {RemainingDetailClosureStatus::parser_independent_required};
  }
  if (registry.controls.empty()) {
    return {RemainingDetailClosureStatus::control_records_required};
  }
  if (registry.surfaces.empty()) {
    return {RemainingDetailClosureStatus::surface_records_required};
  }
  if (registry.gates.empty()) {
    return {RemainingDetailClosureStatus::gate_records_required};
  }
  constexpr RemainingDetailClosureControlKind required_controls[] = {
      RemainingDetailClosureControlKind::system_catalog_schema,
      RemainingDetailClosureControlKind::type_security_privilege,
      RemainingDetailClosureControlKind::diagnostic_registry,
      RemainingDetailClosureControlKind::resource_registry_versioning,
      RemainingDetailClosureControlKind::parser_sblr_grammar_expansion,
      RemainingDetailClosureControlKind::reference_specific_type_matrices,
      RemainingDetailClosureControlKind::canonical_encoding_examples,
      RemainingDetailClosureControlKind::compatibility_mode_matrix,
      RemainingDetailClosureControlKind::conformance_test_manifest_format,
      RemainingDetailClosureControlKind::documentation_examples_policy,
      RemainingDetailClosureControlKind::system_catalog_table_definitions,
      RemainingDetailClosureControlKind::parser_sblr_formal_grammar,
      RemainingDetailClosureControlKind::normative_encoding_examples,
      RemainingDetailClosureControlKind::conformance_manifest_inventory,
      RemainingDetailClosureControlKind::reference_version_profile_closure,
      RemainingDetailClosureControlKind::commercial_grade_completion_gates};
  for (const auto kind : required_controls) {
    if (!RemainingDetailClosureHasControlKind(registry, kind)) {
      return {RemainingDetailClosureStatus::control_record_missing};
    }
  }
  for (std::size_t surface_index = 0; surface_index < registry.surfaces.size();
       ++surface_index) {
    const auto surface_result = ValidateRemainingDetailClosureSurface(
        registry, registry.surfaces[surface_index], surface_index);
    if (!surface_result.ok()) {
      return surface_result;
    }
  }
  for (std::size_t gate_index = 0; gate_index < registry.gates.size();
       ++gate_index) {
    const auto gate_result = ValidateRemainingDetailClosureGate(
        registry, registry.gates[gate_index], gate_index);
    if (!gate_result.ok()) {
      return gate_result;
    }
  }
  for (std::size_t control_index = 0;
       control_index < registry.controls.size(); ++control_index) {
    const auto control_result = ValidateRemainingDetailClosureControl(
        registry, registry.controls[control_index], control_index);
    if (!control_result.ok()) {
      return control_result;
    }
  }
  if (!registry.diagnostics_root_declared) {
    return {RemainingDetailClosureStatus::diagnostic_vector_missing};
  }
  constexpr std::string_view required_diagnostics[] = {
      "RDC.CONTROL_RECORD_MISSING",
      "RDC.CONTROL_IDENTITY_INCOMPLETE",
      "RDC.SURFACE_RECORD_MISSING",
      "RDC.SURFACE_INCOMPLETE",
      "RDC.GATE_RECORD_MISSING",
      "RDC.GATE_EVIDENCE_INCOMPLETE",
      "RDC.PARSER_AUTHORITY_VIOLATION",
      "RDC.SECURITY_POLICY_MISSING",
      "RDC.RESOURCE_INVALIDATION_MISSING",
      "RDC.SILENT_FALLBACK_ALLOWED",
      "RDC.EXAMPLE_AUTHORITY_VIOLATION"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!RemainingDetailClosureHasDiagnosticCode(registry,
                                                 diagnostic_code)) {
      return {RemainingDetailClosureStatus::diagnostic_vector_missing};
    }
  }
  if (!registry.local_metrics_root_declared) {
    return {RemainingDetailClosureStatus::local_metrics_root_required};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.remaining_detail_closure.control_count",
      "sys.metrics.remaining_detail_closure.surface_count",
      "sys.metrics.remaining_detail_closure.gate_count",
      "sys.metrics.remaining_detail_closure.control_missing_total",
      "sys.metrics.remaining_detail_closure.surface_missing_total",
      "sys.metrics.remaining_detail_closure.surface_incomplete_total",
      "sys.metrics.remaining_detail_closure.gate_missing_total",
      "sys.metrics.remaining_detail_closure.gate_evidence_incomplete_total",
      "sys.metrics.remaining_detail_closure.parser_authority_violation_total",
      "sys.metrics.remaining_detail_closure.security_policy_missing_total",
      "sys.metrics.remaining_detail_closure.resource_invalidation_missing_total",
      "sys.metrics.remaining_detail_closure.silent_fallback_allowed_total",
      "sys.metrics.remaining_detail_closure.example_authority_violation_total"};
  for (std::string_view metric : required_metrics) {
    if (!RemainingDetailClosureHasMetric(registry, metric)) {
      return {RemainingDetailClosureStatus::local_metric_missing};
    }
  }
  return {};
}

// SEARCH_KEY: CGC-COMMERCIAL-GRADE-COMPLETION-GATES
// SEARCH_KEY: CGC-SPECIFICATION-COMPLETE-GATES
// SEARCH_KEY: CGC-IMPLEMENTATION-MAPPED-GATES
// SEARCH_KEY: CGC-FINAL-CONTRADICTION-CHECKLIST
// SEARCH_KEY: CGC-AUDIT-MATRIX
// SEARCH_KEY: CGC-GATE-001
// SEARCH_KEY: CGC-GATE-025
enum class CommercialGradeCompletionLevel : std::uint8_t {
  not_started = 0,
  architecture_present = 1,
  specification_expanded = 2,
  cross_reconciled = 3,
  implementation_mapped = 4,
  conformance_ready = 5,
  release_candidate = 6,
  commercial_grade_complete = 7,
  blocked = 8
};

constexpr bool CommercialGradeCompletionLevelIsValid(
    CommercialGradeCompletionLevel level) noexcept {
  switch (level) {
    case CommercialGradeCompletionLevel::not_started:
    case CommercialGradeCompletionLevel::architecture_present:
    case CommercialGradeCompletionLevel::specification_expanded:
    case CommercialGradeCompletionLevel::cross_reconciled:
    case CommercialGradeCompletionLevel::implementation_mapped:
    case CommercialGradeCompletionLevel::conformance_ready:
    case CommercialGradeCompletionLevel::release_candidate:
    case CommercialGradeCompletionLevel::commercial_grade_complete:
    case CommercialGradeCompletionLevel::blocked:
      return true;
  }
  return false;
}

constexpr bool CommercialGradeLevelAtLeast(
    CommercialGradeCompletionLevel level,
    CommercialGradeCompletionLevel minimum) noexcept {
  return static_cast<std::uint8_t>(level) >=
         static_cast<std::uint8_t>(minimum);
}

enum class CommercialGradeRequirementKind : std::uint8_t {
  record = 0,
  field = 1,
  algorithm = 2,
  decision_branch = 3,
  catalog_object = 4,
  metric = 5,
  setting = 6,
  diagnostic = 7,
  conformance_gate = 8,
  security_rule = 9,
  transaction_rule = 10,
  cluster_rule = 11,
  cache_rule = 12,
  unsupported_rule = 13,
  deferred_rule = 14,
  example = 15
};

constexpr bool CommercialGradeRequirementKindIsValid(
    CommercialGradeRequirementKind kind) noexcept {
  switch (kind) {
    case CommercialGradeRequirementKind::record:
    case CommercialGradeRequirementKind::field:
    case CommercialGradeRequirementKind::algorithm:
    case CommercialGradeRequirementKind::decision_branch:
    case CommercialGradeRequirementKind::catalog_object:
    case CommercialGradeRequirementKind::metric:
    case CommercialGradeRequirementKind::setting:
    case CommercialGradeRequirementKind::diagnostic:
    case CommercialGradeRequirementKind::conformance_gate:
    case CommercialGradeRequirementKind::security_rule:
    case CommercialGradeRequirementKind::transaction_rule:
    case CommercialGradeRequirementKind::cluster_rule:
    case CommercialGradeRequirementKind::cache_rule:
    case CommercialGradeRequirementKind::unsupported_rule:
    case CommercialGradeRequirementKind::deferred_rule:
    case CommercialGradeRequirementKind::example:
      return true;
  }
  return false;
}

enum class CommercialGradeDependencyKind : std::uint8_t {
  uses_record = 0,
  uses_algorithm = 1,
  uses_policy = 2,
  uses_metric = 3,
  uses_diagnostic = 4,
  uses_conformance_gate = 5,
  overrides = 6,
  forbids = 7,
  defers_to = 8,
  must_not_conflict = 9
};

constexpr bool CommercialGradeDependencyKindIsValid(
    CommercialGradeDependencyKind kind) noexcept {
  switch (kind) {
    case CommercialGradeDependencyKind::uses_record:
    case CommercialGradeDependencyKind::uses_algorithm:
    case CommercialGradeDependencyKind::uses_policy:
    case CommercialGradeDependencyKind::uses_metric:
    case CommercialGradeDependencyKind::uses_diagnostic:
    case CommercialGradeDependencyKind::uses_conformance_gate:
    case CommercialGradeDependencyKind::overrides:
    case CommercialGradeDependencyKind::forbids:
    case CommercialGradeDependencyKind::defers_to:
    case CommercialGradeDependencyKind::must_not_conflict:
      return true;
  }
  return false;
}

enum class CommercialGradeDependencyResolution : std::uint8_t {
  resolved = 0,
  deferred = 1,
  unsupported = 2,
  contradiction = 3,
  blocked = 4
};

constexpr bool CommercialGradeDependencyResolutionIsValid(
    CommercialGradeDependencyResolution resolution) noexcept {
  switch (resolution) {
    case CommercialGradeDependencyResolution::resolved:
    case CommercialGradeDependencyResolution::deferred:
    case CommercialGradeDependencyResolution::unsupported:
    case CommercialGradeDependencyResolution::contradiction:
    case CommercialGradeDependencyResolution::blocked:
      return true;
  }
  return false;
}

enum class CommercialGradeImplementationStatus : std::uint8_t {
  not_started = 0,
  partial = 1,
  implemented = 2,
  verified = 3,
  deferred = 4,
  unsupported = 5,
  blocked = 6
};

constexpr bool CommercialGradeImplementationStatusIsValid(
    CommercialGradeImplementationStatus status) noexcept {
  switch (status) {
    case CommercialGradeImplementationStatus::not_started:
    case CommercialGradeImplementationStatus::partial:
    case CommercialGradeImplementationStatus::implemented:
    case CommercialGradeImplementationStatus::verified:
    case CommercialGradeImplementationStatus::deferred:
    case CommercialGradeImplementationStatus::unsupported:
    case CommercialGradeImplementationStatus::blocked:
      return true;
  }
  return false;
}

enum class CommercialGradeUnsupportedDeferredState : std::uint8_t {
  unsupported = 0,
  deferred = 1,
  experimental = 2,
  forbidden = 3
};

constexpr bool CommercialGradeUnsupportedDeferredStateIsValid(
    CommercialGradeUnsupportedDeferredState state) noexcept {
  switch (state) {
    case CommercialGradeUnsupportedDeferredState::unsupported:
    case CommercialGradeUnsupportedDeferredState::deferred:
    case CommercialGradeUnsupportedDeferredState::experimental:
    case CommercialGradeUnsupportedDeferredState::forbidden:
      return true;
  }
  return false;
}

enum class CommercialGradeContradictionKind : std::uint8_t {
  authority_conflict = 0,
  field_conflict = 1,
  algorithm_conflict = 2,
  diagnostic_conflict = 3,
  metric_conflict = 4,
  schema_path_conflict = 5,
  transaction_conflict = 6,
  security_conflict = 7,
  cluster_conflict = 8,
  reference_conflict = 9,
  unsupported_conflict = 10
};

constexpr bool CommercialGradeContradictionKindIsValid(
    CommercialGradeContradictionKind kind) noexcept {
  switch (kind) {
    case CommercialGradeContradictionKind::authority_conflict:
    case CommercialGradeContradictionKind::field_conflict:
    case CommercialGradeContradictionKind::algorithm_conflict:
    case CommercialGradeContradictionKind::diagnostic_conflict:
    case CommercialGradeContradictionKind::metric_conflict:
    case CommercialGradeContradictionKind::schema_path_conflict:
    case CommercialGradeContradictionKind::transaction_conflict:
    case CommercialGradeContradictionKind::security_conflict:
    case CommercialGradeContradictionKind::cluster_conflict:
    case CommercialGradeContradictionKind::reference_conflict:
    case CommercialGradeContradictionKind::unsupported_conflict:
      return true;
  }
  return false;
}

enum class CommercialGradeContradictionSeverity : std::uint8_t {
  blocking = 0,
  major = 1,
  minor = 2
};

constexpr bool CommercialGradeContradictionSeverityIsValid(
    CommercialGradeContradictionSeverity severity) noexcept {
  switch (severity) {
    case CommercialGradeContradictionSeverity::blocking:
    case CommercialGradeContradictionSeverity::major:
    case CommercialGradeContradictionSeverity::minor:
      return true;
  }
  return false;
}

enum class CommercialGradeContradictionResolution : std::uint8_t {
  open = 0,
  resolved = 1,
  deferred_with_owner = 2,
  accepted_forbidden_behavior = 3,
  blocked = 4
};

constexpr bool CommercialGradeContradictionResolutionIsValid(
    CommercialGradeContradictionResolution resolution) noexcept {
  switch (resolution) {
    case CommercialGradeContradictionResolution::open:
    case CommercialGradeContradictionResolution::resolved:
    case CommercialGradeContradictionResolution::deferred_with_owner:
    case CommercialGradeContradictionResolution::accepted_forbidden_behavior:
    case CommercialGradeContradictionResolution::blocked:
      return true;
  }
  return false;
}

enum class CommercialGradeGateDecision : std::uint8_t {
  go = 0,
  no_go = 1,
  conditional_go = 2,
  not_evaluated = 3
};

constexpr bool CommercialGradeGateDecisionIsValid(
    CommercialGradeGateDecision decision) noexcept {
  switch (decision) {
    case CommercialGradeGateDecision::go:
    case CommercialGradeGateDecision::no_go:
    case CommercialGradeGateDecision::conditional_go:
    case CommercialGradeGateDecision::not_evaluated:
      return true;
  }
  return false;
}

struct CommercialGradeRequirementRecord {
  Uuid requirement_uuid{};
  std::string requirement_key;
  std::string source_spec_path;
  std::string source_search_key;
  std::string subsystem;
  CommercialGradeRequirementKind requirement_kind =
      CommercialGradeRequirementKind::record;
  std::string normative_text_hash;
  std::string dependency_set_hash;
  CommercialGradeCompletionLevel status =
      CommercialGradeCompletionLevel::not_started;
  Uuid unsupported_or_deferred_uuid{};
  Uuid diagnostic_profile_uuid{};
  Uuid conformance_gate_uuid{};
  std::string record_hash;
};

struct CommercialGradeFieldCompletenessRecord {
  Uuid field_record_uuid{};
  Uuid requirement_uuid{};
  std::string field_name;
  std::string field_type;
  std::string units;
  std::string valid_range;
  std::string default_value;
  std::string authority;
  std::string invalid_state_behavior;
  std::string field_hash;
};

struct CommercialGradeAlgorithmRecord {
  Uuid algorithm_uuid{};
  Uuid requirement_uuid{};
  std::string algorithm_name;
  std::string input_contract_hash;
  std::string output_contract_hash;
  std::uint32_t step_count = 0;
  std::string decision_branch_set_hash;
  std::string failure_diagnostic_set_hash;
  std::string side_effect_set_hash;
  std::string algorithm_hash;
};

struct CommercialGradeCrossSpecDependencyRecord {
  Uuid dependency_uuid{};
  Uuid source_requirement_uuid{};
  Uuid target_requirement_uuid{};
  CommercialGradeDependencyKind dependency_kind =
      CommercialGradeDependencyKind::uses_record;
  CommercialGradeDependencyResolution resolution_state =
      CommercialGradeDependencyResolution::blocked;
  std::string resolution_search_key;
  std::string dependency_hash;
};

struct CommercialGradeImplementationMappingRecord {
  Uuid implementation_mapping_uuid{};
  Uuid requirement_uuid{};
  std::string implementation_path;
  std::string implementation_search_key;
  CommercialGradeImplementationStatus implementation_status =
      CommercialGradeImplementationStatus::not_started;
  Uuid conformance_gate_uuid{};
  std::string evidence_path;
  std::string mapping_hash;
};

struct CommercialGradeUnsupportedDeferredRecord {
  Uuid unsupported_or_deferred_uuid{};
  Uuid requirement_uuid{};
  CommercialGradeUnsupportedDeferredState state =
      CommercialGradeUnsupportedDeferredState::deferred;
  std::string reason;
  std::string risk;
  std::string owner;
  std::string activation_criteria;
  std::string diagnostic_code;
  std::string client_metadata_requirement;
  std::string record_hash;
};

struct CommercialGradeContradictionRecord {
  Uuid contradiction_uuid{};
  Uuid first_requirement_uuid{};
  Uuid second_requirement_uuid{};
  CommercialGradeContradictionKind contradiction_kind =
      CommercialGradeContradictionKind::authority_conflict;
  CommercialGradeContradictionSeverity severity =
      CommercialGradeContradictionSeverity::blocking;
  CommercialGradeContradictionResolution resolution_state =
      CommercialGradeContradictionResolution::open;
  std::string resolution_search_key;
  std::string record_hash;
};

struct CommercialGradeGateDecisionRecord {
  Uuid gate_decision_uuid{};
  Uuid scope_uuid{};
  std::string scope_path;
  CommercialGradeCompletionLevel requested_level =
      CommercialGradeCompletionLevel::not_started;
  std::string evaluated_requirement_set_hash;
  std::string passed_gate_set_hash;
  std::string failed_gate_set_hash;
  std::string blocking_contradiction_set_hash;
  CommercialGradeGateDecision decision =
      CommercialGradeGateDecision::not_evaluated;
  std::string decision_rationale_search_key;
  std::string decision_hash;
};

struct CommercialGradeCompletionEvidenceRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string registry_name;
  std::string root_search_key;
  CommercialGradeCompletionLevel requested_level =
      CommercialGradeCompletionLevel::release_candidate;
  std::vector<CommercialGradeRequirementRecord> requirements;
  std::vector<CommercialGradeFieldCompletenessRecord> fields;
  std::vector<CommercialGradeAlgorithmRecord> algorithms;
  std::vector<CommercialGradeCrossSpecDependencyRecord> dependencies;
  std::vector<CommercialGradeImplementationMappingRecord> mappings;
  std::vector<CommercialGradeUnsupportedDeferredRecord> unsupported_deferred;
  std::vector<CommercialGradeContradictionRecord> contradictions;
  std::vector<CommercialGradeGateDecisionRecord> gate_decisions;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool private_docs_boundary_enforced = true;
  bool descriptor_authority_preserved = true;
  bool uuid_identity_preserved = true;
  bool parser_sblr_boundary_preserved = true;
  bool mga_authority_preserved = true;
  bool examples_non_authoritative = true;
  bool silent_fallback_forbidden = true;
};

enum class CommercialGradeCompletionStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  root_search_key_required = 4,
  requirement_records_required = 5,
  requirement_uuid_required = 6,
  requirement_identity_incomplete = 7,
  requirement_kind_invalid = 8,
  requirement_status_invalid = 9,
  requirement_duplicate = 10,
  source_path_outside_private_docs = 11,
  diagnostic_missing = 12,
  conformance_gate_missing = 13,
  field_completeness_missing = 14,
  field_identity_incomplete = 15,
  algorithm_incomplete = 16,
  dependency_unresolved = 17,
  implementation_mapping_missing = 18,
  implementation_mapping_incomplete = 19,
  unsupported_deferred_missing = 20,
  unsupported_deferred_incomplete = 21,
  contradiction_open = 22,
  gate_decision_missing = 23,
  gate_decision_invalid = 24,
  release_evidence_missing = 25,
  private_docs_boundary_violation = 26,
  authority_invariant_violation = 27,
  diagnostic_vector_missing = 28,
  local_metric_missing = 29
};

constexpr std::string_view CommercialGradeCompletionStatusName(
    CommercialGradeCompletionStatus status) noexcept {
  switch (status) {
    case CommercialGradeCompletionStatus::ok:
      return "ok";
    case CommercialGradeCompletionStatus::registry_uuid_required:
      return "registry_uuid_required";
    case CommercialGradeCompletionStatus::registry_epoch_required:
      return "registry_epoch_required";
    case CommercialGradeCompletionStatus::registry_name_required:
      return "registry_name_required";
    case CommercialGradeCompletionStatus::root_search_key_required:
      return "root_search_key_required";
    case CommercialGradeCompletionStatus::requirement_records_required:
      return "requirement_records_required";
    case CommercialGradeCompletionStatus::requirement_uuid_required:
      return "requirement_uuid_required";
    case CommercialGradeCompletionStatus::requirement_identity_incomplete:
      return "requirement_identity_incomplete";
    case CommercialGradeCompletionStatus::requirement_kind_invalid:
      return "requirement_kind_invalid";
    case CommercialGradeCompletionStatus::requirement_status_invalid:
      return "requirement_status_invalid";
    case CommercialGradeCompletionStatus::requirement_duplicate:
      return "requirement_duplicate";
    case CommercialGradeCompletionStatus::source_path_outside_private_docs:
      return "source_path_outside_private_docs";
    case CommercialGradeCompletionStatus::diagnostic_missing:
      return "diagnostic_missing";
    case CommercialGradeCompletionStatus::conformance_gate_missing:
      return "conformance_gate_missing";
    case CommercialGradeCompletionStatus::field_completeness_missing:
      return "field_completeness_missing";
    case CommercialGradeCompletionStatus::field_identity_incomplete:
      return "field_identity_incomplete";
    case CommercialGradeCompletionStatus::algorithm_incomplete:
      return "algorithm_incomplete";
    case CommercialGradeCompletionStatus::dependency_unresolved:
      return "dependency_unresolved";
    case CommercialGradeCompletionStatus::implementation_mapping_missing:
      return "implementation_mapping_missing";
    case CommercialGradeCompletionStatus::implementation_mapping_incomplete:
      return "implementation_mapping_incomplete";
    case CommercialGradeCompletionStatus::unsupported_deferred_missing:
      return "unsupported_deferred_missing";
    case CommercialGradeCompletionStatus::unsupported_deferred_incomplete:
      return "unsupported_deferred_incomplete";
    case CommercialGradeCompletionStatus::contradiction_open:
      return "contradiction_open";
    case CommercialGradeCompletionStatus::gate_decision_missing:
      return "gate_decision_missing";
    case CommercialGradeCompletionStatus::gate_decision_invalid:
      return "gate_decision_invalid";
    case CommercialGradeCompletionStatus::release_evidence_missing:
      return "release_evidence_missing";
    case CommercialGradeCompletionStatus::private_docs_boundary_violation:
      return "private_docs_boundary_violation";
    case CommercialGradeCompletionStatus::authority_invariant_violation:
      return "authority_invariant_violation";
    case CommercialGradeCompletionStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case CommercialGradeCompletionStatus::local_metric_missing:
      return "local_metric_missing";
  }
  return "unknown_status";
}

struct CommercialGradeCompletionValidationResult {
  CommercialGradeCompletionStatus status = CommercialGradeCompletionStatus::ok;
  std::size_t requirement_index = 0;
  std::size_t field_index = 0;
  std::size_t algorithm_index = 0;
  std::size_t dependency_index = 0;
  std::size_t mapping_index = 0;
  std::size_t unsupported_deferred_index = 0;
  std::size_t contradiction_index = 0;
  std::size_t gate_decision_index = 0;

  bool ok() const noexcept {
    return status == CommercialGradeCompletionStatus::ok;
  }
};

inline const CommercialGradeRequirementRecord*
CommercialGradeFindRequirementByUuid(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const Uuid& requirement_uuid) noexcept {
  for (const auto& requirement : registry.requirements) {
    if (ExecutionDataPacketUuidEquals(requirement.requirement_uuid,
                                      requirement_uuid)) {
      return &requirement;
    }
  }
  return nullptr;
}

inline bool CommercialGradePrivatePathAllowed(
    std::string_view path) noexcept {
  return path.rfind("docs/" "specifications/", 0) == 0 ||
         path.rfind("docs/reference/", 0) == 0 ||
         path.rfind("docs/migration/", 0) == 0 ||
         path.rfind("project/", 0) == 0;
}

inline bool CommercialGradeHasFieldRecord(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const Uuid& requirement_uuid) noexcept {
  for (const auto& field : registry.fields) {
    if (ExecutionDataPacketUuidEquals(field.requirement_uuid,
                                      requirement_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CommercialGradeHasAlgorithmRecord(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const Uuid& requirement_uuid) noexcept {
  for (const auto& algorithm : registry.algorithms) {
    if (ExecutionDataPacketUuidEquals(algorithm.requirement_uuid,
                                      requirement_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CommercialGradeHasMappingRecord(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const Uuid& requirement_uuid) noexcept {
  for (const auto& mapping : registry.mappings) {
    if (ExecutionDataPacketUuidEquals(mapping.requirement_uuid,
                                      requirement_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CommercialGradeHasUnsupportedDeferredRecord(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeRequirementRecord& requirement) noexcept {
  if (ExecutionDataPacketUuidIsNil(requirement.unsupported_or_deferred_uuid)) {
    return false;
  }
  for (const auto& record : registry.unsupported_deferred) {
    if (ExecutionDataPacketUuidEquals(
            record.unsupported_or_deferred_uuid,
            requirement.unsupported_or_deferred_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CommercialGradeHasDiagnosticCode(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

inline bool CommercialGradeHasMetric(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeRequirement(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeRequirementRecord& requirement,
    std::size_t requirement_index) {
  if (ExecutionDataPacketUuidIsNil(requirement.requirement_uuid)) {
    return {CommercialGradeCompletionStatus::requirement_uuid_required,
            requirement_index};
  }
  if (requirement.requirement_key.empty() ||
      requirement.source_spec_path.empty() ||
      requirement.source_search_key.empty() ||
      requirement.subsystem.empty() ||
      requirement.normative_text_hash.empty() ||
      requirement.dependency_set_hash.empty() ||
      requirement.record_hash.empty()) {
    return {CommercialGradeCompletionStatus::requirement_identity_incomplete,
            requirement_index};
  }
  if (!CommercialGradePrivatePathAllowed(requirement.source_spec_path)) {
    return {CommercialGradeCompletionStatus::source_path_outside_private_docs,
            requirement_index};
  }
  if (!CommercialGradeRequirementKindIsValid(
          requirement.requirement_kind)) {
    return {CommercialGradeCompletionStatus::requirement_kind_invalid,
            requirement_index};
  }
  if (!CommercialGradeCompletionLevelIsValid(requirement.status)) {
    return {CommercialGradeCompletionStatus::requirement_status_invalid,
            requirement_index};
  }
  if (ExecutionDataPacketUuidIsNil(requirement.diagnostic_profile_uuid)) {
    return {CommercialGradeCompletionStatus::diagnostic_missing,
            requirement_index};
  }
  if (CommercialGradeLevelAtLeast(requirement.status,
                                  CommercialGradeCompletionLevel::
                                      conformance_ready) &&
      ExecutionDataPacketUuidIsNil(requirement.conformance_gate_uuid)) {
    return {CommercialGradeCompletionStatus::conformance_gate_missing,
            requirement_index};
  }
  if ((requirement.status == CommercialGradeCompletionLevel::blocked ||
       requirement.requirement_kind ==
           CommercialGradeRequirementKind::unsupported_rule ||
       requirement.requirement_kind ==
           CommercialGradeRequirementKind::deferred_rule) &&
      !CommercialGradeHasUnsupportedDeferredRecord(registry, requirement)) {
    return {CommercialGradeCompletionStatus::unsupported_deferred_missing,
            requirement_index};
  }
  if (!CommercialGradeHasFieldRecord(registry,
                                     requirement.requirement_uuid)) {
    return {CommercialGradeCompletionStatus::field_completeness_missing,
            requirement_index};
  }
  if (!CommercialGradeHasAlgorithmRecord(registry,
                                         requirement.requirement_uuid)) {
    return {CommercialGradeCompletionStatus::algorithm_incomplete,
            requirement_index};
  }
  if (CommercialGradeLevelAtLeast(requirement.status,
                                  CommercialGradeCompletionLevel::
                                      implementation_mapped) &&
      !CommercialGradeHasMappingRecord(registry,
                                       requirement.requirement_uuid)) {
    return {CommercialGradeCompletionStatus::implementation_mapping_missing,
            requirement_index};
  }
  for (std::size_t other_index = requirement_index + 1;
       other_index < registry.requirements.size(); ++other_index) {
    const auto& other = registry.requirements[other_index];
    if (requirement.requirement_key == other.requirement_key ||
        ExecutionDataPacketUuidEquals(requirement.requirement_uuid,
                                      other.requirement_uuid)) {
      return {CommercialGradeCompletionStatus::requirement_duplicate,
              other_index};
    }
  }
  return {};
}

inline CommercialGradeCompletionValidationResult ValidateCommercialGradeField(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeFieldCompletenessRecord& field,
    std::size_t field_index) {
  if (ExecutionDataPacketUuidIsNil(field.field_record_uuid) ||
      CommercialGradeFindRequirementByUuid(registry,
                                           field.requirement_uuid) == nullptr ||
      field.field_name.empty() || field.field_type.empty() ||
      field.units.empty() || field.valid_range.empty() ||
      field.default_value.empty() || field.authority.empty() ||
      field.invalid_state_behavior.empty() || field.field_hash.empty()) {
    return {CommercialGradeCompletionStatus::field_identity_incomplete, 0,
            field_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeAlgorithm(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeAlgorithmRecord& algorithm,
    std::size_t algorithm_index) {
  if (ExecutionDataPacketUuidIsNil(algorithm.algorithm_uuid) ||
      CommercialGradeFindRequirementByUuid(
          registry, algorithm.requirement_uuid) == nullptr ||
      algorithm.algorithm_name.empty() ||
      algorithm.input_contract_hash.empty() ||
      algorithm.output_contract_hash.empty() || algorithm.step_count == 0 ||
      algorithm.decision_branch_set_hash.empty() ||
      algorithm.failure_diagnostic_set_hash.empty() ||
      algorithm.side_effect_set_hash.empty() ||
      algorithm.algorithm_hash.empty()) {
    return {CommercialGradeCompletionStatus::algorithm_incomplete, 0, 0,
            algorithm_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeDependency(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeCrossSpecDependencyRecord& dependency,
    std::size_t dependency_index) {
  if (ExecutionDataPacketUuidIsNil(dependency.dependency_uuid) ||
      CommercialGradeFindRequirementByUuid(
          registry, dependency.source_requirement_uuid) == nullptr ||
      CommercialGradeFindRequirementByUuid(
          registry, dependency.target_requirement_uuid) == nullptr ||
      dependency.resolution_search_key.empty() ||
      dependency.dependency_hash.empty() ||
      !CommercialGradeDependencyKindIsValid(dependency.dependency_kind) ||
      !CommercialGradeDependencyResolutionIsValid(
          dependency.resolution_state) ||
      dependency.resolution_state ==
          CommercialGradeDependencyResolution::contradiction ||
      dependency.resolution_state ==
          CommercialGradeDependencyResolution::blocked) {
    return {CommercialGradeCompletionStatus::dependency_unresolved, 0, 0, 0,
            dependency_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeMapping(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeImplementationMappingRecord& mapping,
    std::size_t mapping_index) {
  const auto* requirement =
      CommercialGradeFindRequirementByUuid(registry, mapping.requirement_uuid);
  if (ExecutionDataPacketUuidIsNil(mapping.implementation_mapping_uuid) ||
      requirement == nullptr || mapping.implementation_path.empty() ||
      mapping.implementation_search_key.empty() ||
      mapping.mapping_hash.empty() ||
      !CommercialGradeImplementationStatusIsValid(
          mapping.implementation_status)) {
    return {CommercialGradeCompletionStatus::implementation_mapping_incomplete,
            0, 0, 0, 0, mapping_index};
  }
  if (CommercialGradeLevelAtLeast(requirement->status,
                                  CommercialGradeCompletionLevel::
                                      conformance_ready) &&
      ExecutionDataPacketUuidIsNil(mapping.conformance_gate_uuid)) {
    return {CommercialGradeCompletionStatus::conformance_gate_missing, 0, 0,
            0, 0, mapping_index};
  }
  if (CommercialGradeLevelAtLeast(requirement->status,
                                  CommercialGradeCompletionLevel::
                                      release_candidate) &&
      (mapping.implementation_status !=
           CommercialGradeImplementationStatus::verified ||
       mapping.evidence_path.empty() ||
       !CommercialGradePrivatePathAllowed(mapping.evidence_path))) {
    return {CommercialGradeCompletionStatus::release_evidence_missing, 0, 0,
            0, 0, mapping_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeUnsupportedDeferred(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeUnsupportedDeferredRecord& record,
    std::size_t record_index) {
  if (ExecutionDataPacketUuidIsNil(record.unsupported_or_deferred_uuid) ||
      CommercialGradeFindRequirementByUuid(registry,
                                           record.requirement_uuid) ==
          nullptr ||
      !CommercialGradeUnsupportedDeferredStateIsValid(record.state) ||
      record.reason.empty() || record.risk.empty() ||
      record.diagnostic_code.empty() ||
      record.client_metadata_requirement.empty() || record.record_hash.empty()) {
    return {CommercialGradeCompletionStatus::unsupported_deferred_incomplete,
            0, 0, 0, 0, 0, record_index};
  }
  if ((record.state == CommercialGradeUnsupportedDeferredState::deferred ||
       record.state ==
           CommercialGradeUnsupportedDeferredState::experimental) &&
      (record.owner.empty() || record.activation_criteria.empty())) {
    return {CommercialGradeCompletionStatus::unsupported_deferred_incomplete,
            0, 0, 0, 0, 0, record_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeContradiction(
    const CommercialGradeCompletionEvidenceRegistry& registry,
    const CommercialGradeContradictionRecord& contradiction,
    std::size_t contradiction_index) {
  if (ExecutionDataPacketUuidIsNil(contradiction.contradiction_uuid) ||
      CommercialGradeFindRequirementByUuid(
          registry, contradiction.first_requirement_uuid) == nullptr ||
      CommercialGradeFindRequirementByUuid(
          registry, contradiction.second_requirement_uuid) == nullptr ||
      !CommercialGradeContradictionKindIsValid(
          contradiction.contradiction_kind) ||
      !CommercialGradeContradictionSeverityIsValid(contradiction.severity) ||
      !CommercialGradeContradictionResolutionIsValid(
          contradiction.resolution_state) ||
      contradiction.resolution_search_key.empty() ||
      contradiction.record_hash.empty()) {
    return {CommercialGradeCompletionStatus::contradiction_open, 0, 0, 0, 0,
            0, 0, contradiction_index};
  }
  if ((contradiction.severity ==
           CommercialGradeContradictionSeverity::blocking ||
       contradiction.severity ==
           CommercialGradeContradictionSeverity::major) &&
      (contradiction.resolution_state ==
           CommercialGradeContradictionResolution::open ||
       contradiction.resolution_state ==
           CommercialGradeContradictionResolution::blocked)) {
    return {CommercialGradeCompletionStatus::contradiction_open, 0, 0, 0, 0,
            0, 0, contradiction_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeGateDecision(
    const CommercialGradeGateDecisionRecord& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(decision.gate_decision_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.scope_uuid) ||
      decision.scope_path.empty() ||
      !CommercialGradeCompletionLevelIsValid(decision.requested_level) ||
      decision.evaluated_requirement_set_hash.empty() ||
      decision.passed_gate_set_hash.empty() ||
      decision.failed_gate_set_hash.empty() ||
      decision.blocking_contradiction_set_hash.empty() ||
      !CommercialGradeGateDecisionIsValid(decision.decision) ||
      decision.decision_rationale_search_key.empty() ||
      decision.decision_hash.empty()) {
    return {CommercialGradeCompletionStatus::gate_decision_invalid, 0, 0, 0,
            0, 0, 0, 0, decision_index};
  }
  if (CommercialGradeLevelAtLeast(decision.requested_level,
                                  CommercialGradeCompletionLevel::
                                      release_candidate) &&
      (decision.decision != CommercialGradeGateDecision::go ||
       decision.failed_gate_set_hash != "empty-set" ||
       decision.blocking_contradiction_set_hash != "empty-set")) {
    return {CommercialGradeCompletionStatus::gate_decision_missing, 0, 0, 0,
            0, 0, 0, 0, decision_index};
  }
  return {};
}

inline CommercialGradeCompletionValidationResult
ValidateCommercialGradeCompletionEvidenceRegistry(
    const CommercialGradeCompletionEvidenceRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {CommercialGradeCompletionStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {CommercialGradeCompletionStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {CommercialGradeCompletionStatus::registry_name_required};
  }
  if (registry.root_search_key !=
      "CGC-COMMERCIAL-GRADE-COMPLETION-GATES") {
    return {CommercialGradeCompletionStatus::root_search_key_required};
  }
  if (registry.requirements.empty()) {
    return {CommercialGradeCompletionStatus::requirement_records_required};
  }
  if (!registry.private_docs_boundary_enforced) {
    return {CommercialGradeCompletionStatus::private_docs_boundary_violation};
  }
  if (!registry.descriptor_authority_preserved ||
      !registry.uuid_identity_preserved ||
      !registry.parser_sblr_boundary_preserved ||
      !registry.mga_authority_preserved ||
      !registry.examples_non_authoritative ||
      !registry.silent_fallback_forbidden) {
    return {CommercialGradeCompletionStatus::authority_invariant_violation};
  }
  for (std::size_t field_index = 0; field_index < registry.fields.size();
       ++field_index) {
    const auto field_result =
        ValidateCommercialGradeField(registry, registry.fields[field_index],
                                     field_index);
    if (!field_result.ok()) {
      return field_result;
    }
  }
  for (std::size_t algorithm_index = 0;
       algorithm_index < registry.algorithms.size(); ++algorithm_index) {
    const auto algorithm_result = ValidateCommercialGradeAlgorithm(
        registry, registry.algorithms[algorithm_index], algorithm_index);
    if (!algorithm_result.ok()) {
      return algorithm_result;
    }
  }
  for (std::size_t record_index = 0;
       record_index < registry.unsupported_deferred.size(); ++record_index) {
    const auto record_result = ValidateCommercialGradeUnsupportedDeferred(
        registry, registry.unsupported_deferred[record_index], record_index);
    if (!record_result.ok()) {
      return record_result;
    }
  }
  for (std::size_t dependency_index = 0;
       dependency_index < registry.dependencies.size(); ++dependency_index) {
    const auto dependency_result = ValidateCommercialGradeDependency(
        registry, registry.dependencies[dependency_index], dependency_index);
    if (!dependency_result.ok()) {
      return dependency_result;
    }
  }
  for (std::size_t mapping_index = 0; mapping_index < registry.mappings.size();
       ++mapping_index) {
    const auto mapping_result = ValidateCommercialGradeMapping(
        registry, registry.mappings[mapping_index], mapping_index);
    if (!mapping_result.ok()) {
      return mapping_result;
    }
  }
  for (std::size_t contradiction_index = 0;
       contradiction_index < registry.contradictions.size();
       ++contradiction_index) {
    const auto contradiction_result = ValidateCommercialGradeContradiction(
        registry, registry.contradictions[contradiction_index],
        contradiction_index);
    if (!contradiction_result.ok()) {
      return contradiction_result;
    }
  }
  if (registry.gate_decisions.empty()) {
    return {CommercialGradeCompletionStatus::gate_decision_missing};
  }
  for (std::size_t decision_index = 0;
       decision_index < registry.gate_decisions.size(); ++decision_index) {
    const auto decision_result = ValidateCommercialGradeGateDecision(
        registry.gate_decisions[decision_index], decision_index);
    if (!decision_result.ok()) {
      return decision_result;
    }
  }
  for (std::size_t requirement_index = 0;
       requirement_index < registry.requirements.size(); ++requirement_index) {
    const auto requirement_result = ValidateCommercialGradeRequirement(
        registry, registry.requirements[requirement_index],
        requirement_index);
    if (!requirement_result.ok()) {
      return requirement_result;
    }
  }
  constexpr std::string_view required_diagnostics[] = {
      "CGC.REQUIREMENT_RECORD_MISSING",
      "CGC.SEARCH_KEY_MISSING",
      "CGC.FIELD_COMPLETENESS_MISSING",
      "CGC.ALGORITHM_INCOMPLETE",
      "CGC.DECISION_BRANCH_MISSING",
      "CGC.DIAGNOSTIC_MISSING",
      "CGC.CATALOG_OR_METRIC_UNDEFINED",
      "CGC.CACHE_INVALIDATION_UNDEFINED",
      "CGC.INTERACTION_UNDEFINED",
      "CGC.CONTRADICTION_OPEN",
      "CGC.EXTERNAL_REFERENCE_UNCOPIED",
      "CGC.UNSUPPORTED_BEHAVIOR_UNDECLARED",
      "CGC.DEFERRED_RECORD_INCOMPLETE",
      "CGC.IMPLEMENTATION_MAPPING_MISSING",
      "CGC.CONFORMANCE_GATE_MISSING",
      "CGC.RELEASE_EVIDENCE_MISSING"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!CommercialGradeHasDiagnosticCode(registry, diagnostic_code)) {
      return {CommercialGradeCompletionStatus::diagnostic_vector_missing};
    }
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.commercial_grade.requirement_count",
      "sys.metrics.commercial_grade.field_incomplete_count",
      "sys.metrics.commercial_grade.algorithm_incomplete_count",
      "sys.metrics.commercial_grade.decision_branch_missing_count",
      "sys.metrics.commercial_grade.diagnostic_missing_count",
      "sys.metrics.commercial_grade.catalog_metric_undefined_count",
      "sys.metrics.commercial_grade.cache_invalidation_missing_count",
      "sys.metrics.commercial_grade.open_contradiction_count",
      "sys.metrics.commercial_grade.external_reference_uncopied_count",
      "sys.metrics.commercial_grade.unsupported_undeclared_count",
      "sys.metrics.commercial_grade.implementation_mapping_missing_count",
      "sys.metrics.commercial_grade.conformance_gate_missing_count",
      "sys.metrics.commercial_grade.gate_decision_go_count",
      "sys.metrics.commercial_grade.gate_decision_no_go_count"};
  for (std::string_view metric : required_metrics) {
    if (!CommercialGradeHasMetric(registry, metric)) {
      return {CommercialGradeCompletionStatus::local_metric_missing};
    }
  }
  return {};
}

// SEARCH_KEY: CSD-CANONICAL-SCALAR-DATATYPES
// SEARCH_KEY: CSD-SCALAR-AUTHORITY-RULE
// SEARCH_KEY: CSD-SCALAR-DESCRIPTOR-FIELDS
// SEARCH_KEY: CSD-BOOLEAN
// SEARCH_KEY: CSD-SIGNED-INTEGER
// SEARCH_KEY: CSD-UNSIGNED-INTEGER
// SEARCH_KEY: CSD-EXACT-NUMERIC
// SEARCH_KEY: CSD-DECIMAL-FLOATING
// SEARCH_KEY: CSD-APPROXIMATE-NUMERIC
// SEARCH_KEY: CSD-FLOAT128-BINARY128
// SEARCH_KEY: CSD-MONEY-CURRENCY
// SEARCH_KEY: CSD-SCALAR-LITERAL-BIND-RESOLUTION
// SEARCH_KEY: CSD-SCALAR-CAST-RANKING
// SEARCH_KEY: CSD-SCALAR-OPERATION-FAMILIES
// SEARCH_KEY: CSD-SCALAR-BACKUP-REPLICATION-TRANSPORT
// SEARCH_KEY: CSD-SCALAR-DIAGNOSTICS
enum class CanonicalScalarFamily : std::uint8_t {
  boolean = 0,
  signed_integer = 1,
  unsigned_integer = 2,
  exact_numeric = 3,
  decimal_floating = 4,
  approximate_numeric = 5,
  money_currency = 6
};

constexpr bool CanonicalScalarFamilyIsValid(
    CanonicalScalarFamily family) noexcept {
  switch (family) {
    case CanonicalScalarFamily::boolean:
    case CanonicalScalarFamily::signed_integer:
    case CanonicalScalarFamily::unsigned_integer:
    case CanonicalScalarFamily::exact_numeric:
    case CanonicalScalarFamily::decimal_floating:
    case CanonicalScalarFamily::approximate_numeric:
    case CanonicalScalarFamily::money_currency:
      return true;
  }
  return false;
}

enum class CanonicalScalarType : std::uint8_t {
  boolean = 0,
  int8 = 1,
  int16 = 2,
  int32 = 3,
  int64 = 4,
  int128 = 5,
  uint8 = 6,
  uint16 = 7,
  uint32 = 8,
  uint64 = 9,
  uint128 = 10,
  exact_numeric = 11,
  decimal_floating = 12,
  float32 = 13,
  float64 = 14,
  float128 = 15,
  money_currency = 16
};

constexpr bool CanonicalScalarTypeIsValid(CanonicalScalarType type) noexcept {
  switch (type) {
    case CanonicalScalarType::boolean:
    case CanonicalScalarType::int8:
    case CanonicalScalarType::int16:
    case CanonicalScalarType::int32:
    case CanonicalScalarType::int64:
    case CanonicalScalarType::int128:
    case CanonicalScalarType::uint8:
    case CanonicalScalarType::uint16:
    case CanonicalScalarType::uint32:
    case CanonicalScalarType::uint64:
    case CanonicalScalarType::uint128:
    case CanonicalScalarType::exact_numeric:
    case CanonicalScalarType::decimal_floating:
    case CanonicalScalarType::float32:
    case CanonicalScalarType::float64:
    case CanonicalScalarType::float128:
    case CanonicalScalarType::money_currency:
      return true;
  }
  return false;
}

constexpr CanonicalScalarFamily CanonicalScalarTypeFamily(
    CanonicalScalarType type) noexcept {
  switch (type) {
    case CanonicalScalarType::boolean:
      return CanonicalScalarFamily::boolean;
    case CanonicalScalarType::int8:
    case CanonicalScalarType::int16:
    case CanonicalScalarType::int32:
    case CanonicalScalarType::int64:
    case CanonicalScalarType::int128:
      return CanonicalScalarFamily::signed_integer;
    case CanonicalScalarType::uint8:
    case CanonicalScalarType::uint16:
    case CanonicalScalarType::uint32:
    case CanonicalScalarType::uint64:
    case CanonicalScalarType::uint128:
      return CanonicalScalarFamily::unsigned_integer;
    case CanonicalScalarType::exact_numeric:
      return CanonicalScalarFamily::exact_numeric;
    case CanonicalScalarType::decimal_floating:
      return CanonicalScalarFamily::decimal_floating;
    case CanonicalScalarType::float32:
    case CanonicalScalarType::float64:
    case CanonicalScalarType::float128:
      return CanonicalScalarFamily::approximate_numeric;
    case CanonicalScalarType::money_currency:
      return CanonicalScalarFamily::money_currency;
  }
  return CanonicalScalarFamily::boolean;
}

constexpr std::string_view CanonicalScalarTypeSearchKey(
    CanonicalScalarType type) noexcept {
  switch (type) {
    case CanonicalScalarType::boolean:
      return "CSD-BOOLEAN";
    case CanonicalScalarType::int8:
    case CanonicalScalarType::int16:
    case CanonicalScalarType::int32:
    case CanonicalScalarType::int64:
    case CanonicalScalarType::int128:
      return "CSD-SIGNED-INTEGER";
    case CanonicalScalarType::uint8:
    case CanonicalScalarType::uint16:
    case CanonicalScalarType::uint32:
    case CanonicalScalarType::uint64:
    case CanonicalScalarType::uint128:
      return "CSD-UNSIGNED-INTEGER";
    case CanonicalScalarType::exact_numeric:
      return "CSD-EXACT-NUMERIC";
    case CanonicalScalarType::decimal_floating:
      return "CSD-DECIMAL-FLOATING";
    case CanonicalScalarType::float32:
    case CanonicalScalarType::float64:
      return "CSD-APPROXIMATE-NUMERIC";
    case CanonicalScalarType::float128:
      return "CSD-FLOAT128-BINARY128";
    case CanonicalScalarType::money_currency:
      return "CSD-MONEY-CURRENCY";
  }
  return "";
}

constexpr bool CanonicalScalarTypeRequiresBitWidth(
    CanonicalScalarType type) noexcept {
  switch (type) {
    case CanonicalScalarType::boolean:
    case CanonicalScalarType::int8:
    case CanonicalScalarType::int16:
    case CanonicalScalarType::int32:
    case CanonicalScalarType::int64:
    case CanonicalScalarType::int128:
    case CanonicalScalarType::uint8:
    case CanonicalScalarType::uint16:
    case CanonicalScalarType::uint32:
    case CanonicalScalarType::uint64:
    case CanonicalScalarType::uint128:
    case CanonicalScalarType::float32:
    case CanonicalScalarType::float64:
    case CanonicalScalarType::float128:
      return true;
    case CanonicalScalarType::exact_numeric:
    case CanonicalScalarType::decimal_floating:
    case CanonicalScalarType::money_currency:
      return false;
  }
  return false;
}

constexpr bool CanonicalScalarTypeRequiresPrecision(
    CanonicalScalarType type) noexcept {
  return type != CanonicalScalarType::boolean;
}

constexpr bool CanonicalScalarTypeRequiresScale(
    CanonicalScalarType type) noexcept {
  return type == CanonicalScalarType::exact_numeric ||
         type == CanonicalScalarType::money_currency;
}

constexpr bool CanonicalScalarTypeRequiresNumericContext(
    CanonicalScalarType type) noexcept {
  return type == CanonicalScalarType::decimal_floating ||
         type == CanonicalScalarType::float32 ||
         type == CanonicalScalarType::float64 ||
         type == CanonicalScalarType::float128;
}

enum class CanonicalScalarResolutionKind : std::uint8_t {
  literal = 0,
  bind_parameter = 1
};

constexpr bool CanonicalScalarResolutionKindIsValid(
    CanonicalScalarResolutionKind kind) noexcept {
  switch (kind) {
    case CanonicalScalarResolutionKind::literal:
    case CanonicalScalarResolutionKind::bind_parameter:
      return true;
  }
  return false;
}

enum class CanonicalScalarCastRank : std::uint8_t {
  exact_descriptor_match = 0,
  same_domain_stack_match = 1,
  lossless_same_family_widening = 2,
  lossless_domain_preserving = 3,
  lossless_cross_family = 4,
  assignment_checked = 5,
  explicit_lossy = 6,
  reference_compatibility = 7,
  cpp_udr_bridge = 8
};

constexpr bool CanonicalScalarCastRankIsValid(
    CanonicalScalarCastRank rank) noexcept {
  switch (rank) {
    case CanonicalScalarCastRank::exact_descriptor_match:
    case CanonicalScalarCastRank::same_domain_stack_match:
    case CanonicalScalarCastRank::lossless_same_family_widening:
    case CanonicalScalarCastRank::lossless_domain_preserving:
    case CanonicalScalarCastRank::lossless_cross_family:
    case CanonicalScalarCastRank::assignment_checked:
    case CanonicalScalarCastRank::explicit_lossy:
    case CanonicalScalarCastRank::reference_compatibility:
    case CanonicalScalarCastRank::cpp_udr_bridge:
      return true;
  }
  return false;
}

enum class CanonicalScalarOperationFamily : std::uint8_t {
  cast = 0,
  equality_distinctness = 1,
  ordering = 2,
  hashing = 3,
  arithmetic = 4,
  aggregate = 5,
  window = 6,
  serialization = 7
};

constexpr bool CanonicalScalarOperationFamilyIsValid(
    CanonicalScalarOperationFamily family) noexcept {
  switch (family) {
    case CanonicalScalarOperationFamily::cast:
    case CanonicalScalarOperationFamily::equality_distinctness:
    case CanonicalScalarOperationFamily::ordering:
    case CanonicalScalarOperationFamily::hashing:
    case CanonicalScalarOperationFamily::arithmetic:
    case CanonicalScalarOperationFamily::aggregate:
    case CanonicalScalarOperationFamily::window:
    case CanonicalScalarOperationFamily::serialization:
      return true;
  }
  return false;
}

enum class CanonicalScalarReferenceBehavior : std::uint8_t {
  exact = 0,
  emulated_with_difference = 1,
  unsupported_by_version = 2,
  refused_by_policy = 3
};

constexpr bool CanonicalScalarReferenceBehaviorIsValid(
    CanonicalScalarReferenceBehavior behavior) noexcept {
  switch (behavior) {
    case CanonicalScalarReferenceBehavior::exact:
    case CanonicalScalarReferenceBehavior::emulated_with_difference:
    case CanonicalScalarReferenceBehavior::unsupported_by_version:
    case CanonicalScalarReferenceBehavior::refused_by_policy:
      return true;
  }
  return false;
}

enum class CanonicalScalarGateStatus : std::uint8_t {
  passed = 0,
  failed = 1,
  deferred = 2,
  unsupported_by_policy = 3
};

constexpr bool CanonicalScalarGateStatusIsValid(
    CanonicalScalarGateStatus status) noexcept {
  switch (status) {
    case CanonicalScalarGateStatus::passed:
    case CanonicalScalarGateStatus::failed:
    case CanonicalScalarGateStatus::deferred:
    case CanonicalScalarGateStatus::unsupported_by_policy:
      return true;
  }
  return false;
}

struct ScalarTypeDescriptorRecord {
  Uuid descriptor_uuid{};
  std::string descriptor_key;
  std::string descriptor_search_key;
  CanonicalScalarType canonical_type = CanonicalScalarType::boolean;
  CanonicalScalarFamily scalar_family = CanonicalScalarFamily::boolean;
  std::uint16_t bit_width = 0;
  bool precision_declared = false;
  std::uint16_t precision = 0;
  bool scale_declared = false;
  std::uint16_t scale = 0;
  bool nullable = true;
  Uuid domain_uuid{};
  bool value_state_set_complete = false;
  Uuid numeric_context_uuid{};
  Uuid comparison_contract_uuid{};
  Uuid canonicalization_profile_uuid{};
  Uuid cast_policy_uuid{};
  Uuid operation_policy_uuid{};
  Uuid storage_codec_uuid{};
  Uuid wire_render_policy_uuid{};
  Uuid statistics_profile_uuid{};
  Uuid index_profile_uuid{};
  Uuid backup_transport_profile_uuid{};
  Uuid diagnostic_policy_uuid{};
  Uuid metric_profile_uuid{};
  Uuid conformance_profile_uuid{};
  Uuid amount_descriptor_uuid{};
  Uuid currency_policy_uuid{};
  Uuid monetary_rounding_policy_uuid{};
  Uuid monetary_render_policy_uuid{};
  bool storage_encoding_canonical = false;
  bool raw_host_encoding_forbidden = true;
  std::uint8_t canonical_payload_bytes = 0;
  bool portable_binary128_backend_required = false;
  bool portable_binary128_backend_available = false;
  bool native_acceleration_guarded = true;
  bool silent_downgrade_forbidden = true;
  bool driver_fallback_disclosed = true;
  bool index_statistics_guarded = true;
  bool float128_operations_complete = true;
  std::string descriptor_hash;
};

struct NumericContextRecord {
  Uuid numeric_context_uuid{};
  std::string context_key;
  std::string rounding_mode;
  std::string overflow_policy;
  std::string underflow_policy;
  std::string divide_by_zero_policy;
  std::string nan_policy;
  std::string infinity_policy;
  std::string signed_zero_policy;
  std::string trap_policy;
  std::string total_order_profile;
  bool total_order_required = false;
  std::string context_hash;
};

struct ScalarLiteralBindResolutionRule {
  Uuid resolution_rule_uuid{};
  CanonicalScalarResolutionKind resolution_kind =
      CanonicalScalarResolutionKind::literal;
  std::string resolution_search_key;
  std::string candidate_set_hash;
  std::string ranking_rule_hash;
  std::string resolved_descriptor_evidence_hash;
  std::string ambiguous_diagnostic_code;
  std::string no_match_diagnostic_code;
  bool engine_resolves_final_descriptor = false;
  bool fail_closed_on_ambiguity = false;
  bool driver_metadata_is_hint = false;
  bool parser_spelling_not_authority = false;
  std::string rule_hash;
};

struct ScalarCastOperationRule {
  Uuid rule_uuid{};
  CanonicalScalarOperationFamily operation_family =
      CanonicalScalarOperationFamily::cast;
  CanonicalScalarCastRank cast_rank =
      CanonicalScalarCastRank::exact_descriptor_match;
  Uuid source_descriptor_uuid{};
  Uuid target_descriptor_uuid{};
  Uuid result_descriptor_uuid{};
  std::string operation_search_key;
  std::string operation_policy_hash;
  bool range_check_declared = false;
  bool precision_scale_check_declared = false;
  bool special_value_policy_checked = false;
  bool currency_policy_checked = false;
  bool security_policy_checked = false;
  bool reference_profile_checked = false;
  bool result_descriptor_declared = false;
  bool no_silent_fallback = false;
  std::string failure_diagnostic_code;
  std::string rule_hash;
};

struct ScalarTransportProfileRecord {
  Uuid transport_profile_uuid{};
  Uuid descriptor_uuid{};
  std::string transport_search_key;
  bool backup_manifest_records_descriptor = false;
  bool restore_incompatible_refuses = false;
  bool replication_descriptor_version_checked = false;
  bool cluster_transport_negotiates_descriptor_codec = false;
  bool logical_delta_derivative_only = false;
  bool merge_uses_canonical_equality = false;
  bool driver_metadata_discloses_limitations = false;
  std::string transport_hash;
};

struct ScalarReferenceMappingRecord {
  Uuid reference_mapping_uuid{};
  Uuid descriptor_uuid{};
  std::string reference_profile_key;
  std::string reference_type_name;
  CanonicalScalarReferenceBehavior behavior =
      CanonicalScalarReferenceBehavior::refused_by_policy;
  std::string diagnostic_code;
  std::string mapping_hash;
};

struct CanonicalScalarConformanceGateRecord {
  Uuid gate_uuid{};
  std::string gate_id;
  CanonicalScalarGateStatus status = CanonicalScalarGateStatus::failed;
  std::string evidence_hash;
  std::string ctest_name;
  std::string diagnostic_code;
  std::string gate_hash;
};

struct CanonicalScalarDatatypeRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string registry_name;
  std::string root_search_key;
  std::vector<ScalarTypeDescriptorRecord> descriptors;
  std::vector<NumericContextRecord> numeric_contexts;
  std::vector<ScalarLiteralBindResolutionRule> literal_bind_rules;
  std::vector<ScalarCastOperationRule> cast_operation_rules;
  std::vector<ScalarTransportProfileRecord> transport_profiles;
  std::vector<ScalarReferenceMappingRecord> reference_mappings;
  std::vector<CanonicalScalarConformanceGateRecord> conformance_gates;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool descriptor_authority_preserved = true;
  bool parser_sblr_boundary_preserved = true;
  bool reference_names_not_authority = true;
  bool storage_wire_representation_not_authority = true;
  bool driver_metadata_hint_only = true;
  bool mga_authority_preserved = true;
  bool write_after_delta_not_recovery_authority = true;
  bool silent_float128_downgrade_forbidden = true;
  bool diagnostics_root_declared = true;
  bool local_metrics_root_declared = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
};

enum class CanonicalScalarDatatypeStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  root_search_key_required = 4,
  descriptor_records_required = 5,
  numeric_context_records_required = 6,
  literal_bind_rules_required = 7,
  cast_operation_rules_required = 8,
  transport_profiles_required = 9,
  reference_mapping_records_required = 10,
  conformance_gate_records_required = 11,
  authority_invariant_violation = 12,
  descriptor_record_missing = 13,
  descriptor_uuid_required = 14,
  descriptor_identity_incomplete = 15,
  descriptor_type_invalid = 16,
  descriptor_family_invalid = 17,
  descriptor_family_mismatch = 18,
  descriptor_policy_missing = 19,
  fixed_width_required = 20,
  precision_required = 21,
  scale_required = 22,
  scale_invalid = 23,
  value_state_set_incomplete = 24,
  physical_encoding_invalid = 25,
  raw_host_encoding_allowed = 26,
  numeric_context_missing = 27,
  numeric_context_incomplete = 28,
  money_currency_policy_missing = 29,
  float128_backend_missing = 30,
  float128_encoding_invalid = 31,
  float128_contract_incomplete = 32,
  descriptor_duplicate = 33,
  literal_bind_rule_missing = 34,
  literal_bind_rule_incomplete = 35,
  literal_bind_ambiguity_not_fail_closed = 36,
  cast_operation_rule_missing = 37,
  cast_operation_rule_incomplete = 38,
  cast_operation_checks_incomplete = 39,
  transport_profile_missing = 40,
  transport_profile_incomplete = 41,
  transport_recovery_authority_violation = 42,
  reference_mapping_missing = 43,
  reference_mapping_incomplete = 44,
  conformance_gate_missing = 45,
  conformance_gate_failed = 46,
  diagnostic_vector_missing = 47,
  local_metric_missing = 48,
  cluster_metric_guard_required = 49
};

constexpr std::string_view CanonicalScalarDatatypeStatusName(
    CanonicalScalarDatatypeStatus status) noexcept {
  switch (status) {
    case CanonicalScalarDatatypeStatus::ok:
      return "ok";
    case CanonicalScalarDatatypeStatus::registry_uuid_required:
      return "registry_uuid_required";
    case CanonicalScalarDatatypeStatus::registry_epoch_required:
      return "registry_epoch_required";
    case CanonicalScalarDatatypeStatus::registry_name_required:
      return "registry_name_required";
    case CanonicalScalarDatatypeStatus::root_search_key_required:
      return "root_search_key_required";
    case CanonicalScalarDatatypeStatus::descriptor_records_required:
      return "descriptor_records_required";
    case CanonicalScalarDatatypeStatus::numeric_context_records_required:
      return "numeric_context_records_required";
    case CanonicalScalarDatatypeStatus::literal_bind_rules_required:
      return "literal_bind_rules_required";
    case CanonicalScalarDatatypeStatus::cast_operation_rules_required:
      return "cast_operation_rules_required";
    case CanonicalScalarDatatypeStatus::transport_profiles_required:
      return "transport_profiles_required";
    case CanonicalScalarDatatypeStatus::reference_mapping_records_required:
      return "reference_mapping_records_required";
    case CanonicalScalarDatatypeStatus::conformance_gate_records_required:
      return "conformance_gate_records_required";
    case CanonicalScalarDatatypeStatus::authority_invariant_violation:
      return "authority_invariant_violation";
    case CanonicalScalarDatatypeStatus::descriptor_record_missing:
      return "descriptor_record_missing";
    case CanonicalScalarDatatypeStatus::descriptor_uuid_required:
      return "descriptor_uuid_required";
    case CanonicalScalarDatatypeStatus::descriptor_identity_incomplete:
      return "descriptor_identity_incomplete";
    case CanonicalScalarDatatypeStatus::descriptor_type_invalid:
      return "descriptor_type_invalid";
    case CanonicalScalarDatatypeStatus::descriptor_family_invalid:
      return "descriptor_family_invalid";
    case CanonicalScalarDatatypeStatus::descriptor_family_mismatch:
      return "descriptor_family_mismatch";
    case CanonicalScalarDatatypeStatus::descriptor_policy_missing:
      return "descriptor_policy_missing";
    case CanonicalScalarDatatypeStatus::fixed_width_required:
      return "fixed_width_required";
    case CanonicalScalarDatatypeStatus::precision_required:
      return "precision_required";
    case CanonicalScalarDatatypeStatus::scale_required:
      return "scale_required";
    case CanonicalScalarDatatypeStatus::scale_invalid:
      return "scale_invalid";
    case CanonicalScalarDatatypeStatus::value_state_set_incomplete:
      return "value_state_set_incomplete";
    case CanonicalScalarDatatypeStatus::physical_encoding_invalid:
      return "physical_encoding_invalid";
    case CanonicalScalarDatatypeStatus::raw_host_encoding_allowed:
      return "raw_host_encoding_allowed";
    case CanonicalScalarDatatypeStatus::numeric_context_missing:
      return "numeric_context_missing";
    case CanonicalScalarDatatypeStatus::numeric_context_incomplete:
      return "numeric_context_incomplete";
    case CanonicalScalarDatatypeStatus::money_currency_policy_missing:
      return "money_currency_policy_missing";
    case CanonicalScalarDatatypeStatus::float128_backend_missing:
      return "float128_backend_missing";
    case CanonicalScalarDatatypeStatus::float128_encoding_invalid:
      return "float128_encoding_invalid";
    case CanonicalScalarDatatypeStatus::float128_contract_incomplete:
      return "float128_contract_incomplete";
    case CanonicalScalarDatatypeStatus::descriptor_duplicate:
      return "descriptor_duplicate";
    case CanonicalScalarDatatypeStatus::literal_bind_rule_missing:
      return "literal_bind_rule_missing";
    case CanonicalScalarDatatypeStatus::literal_bind_rule_incomplete:
      return "literal_bind_rule_incomplete";
    case CanonicalScalarDatatypeStatus::
        literal_bind_ambiguity_not_fail_closed:
      return "literal_bind_ambiguity_not_fail_closed";
    case CanonicalScalarDatatypeStatus::cast_operation_rule_missing:
      return "cast_operation_rule_missing";
    case CanonicalScalarDatatypeStatus::cast_operation_rule_incomplete:
      return "cast_operation_rule_incomplete";
    case CanonicalScalarDatatypeStatus::cast_operation_checks_incomplete:
      return "cast_operation_checks_incomplete";
    case CanonicalScalarDatatypeStatus::transport_profile_missing:
      return "transport_profile_missing";
    case CanonicalScalarDatatypeStatus::transport_profile_incomplete:
      return "transport_profile_incomplete";
    case CanonicalScalarDatatypeStatus::transport_recovery_authority_violation:
      return "transport_recovery_authority_violation";
    case CanonicalScalarDatatypeStatus::reference_mapping_missing:
      return "reference_mapping_missing";
    case CanonicalScalarDatatypeStatus::reference_mapping_incomplete:
      return "reference_mapping_incomplete";
    case CanonicalScalarDatatypeStatus::conformance_gate_missing:
      return "conformance_gate_missing";
    case CanonicalScalarDatatypeStatus::conformance_gate_failed:
      return "conformance_gate_failed";
    case CanonicalScalarDatatypeStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case CanonicalScalarDatatypeStatus::local_metric_missing:
      return "local_metric_missing";
    case CanonicalScalarDatatypeStatus::cluster_metric_guard_required:
      return "cluster_metric_guard_required";
  }
  return "unknown";
}

struct CanonicalScalarDatatypeValidationResult {
  CanonicalScalarDatatypeStatus status = CanonicalScalarDatatypeStatus::ok;
  std::size_t descriptor_index = 0;
  std::size_t context_index = 0;
  std::size_t rule_index = 0;
  std::size_t gate_index = 0;

  bool ok() const noexcept {
    return status == CanonicalScalarDatatypeStatus::ok;
  }
};

inline bool CanonicalScalarHasDiagnosticCode(
    const CanonicalScalarDatatypeRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasMetric(
    const CanonicalScalarDatatypeRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline const ScalarTypeDescriptorRecord* CanonicalScalarFindDescriptorByUuid(
    const CanonicalScalarDatatypeRegistry& registry,
    const Uuid& descriptor_uuid) noexcept {
  for (const auto& descriptor : registry.descriptors) {
    if (ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                      descriptor_uuid)) {
      return &descriptor;
    }
  }
  return nullptr;
}

inline bool CanonicalScalarHasDescriptorType(
    const CanonicalScalarDatatypeRegistry& registry,
    CanonicalScalarType type) noexcept {
  for (const auto& descriptor : registry.descriptors) {
    if (descriptor.canonical_type == type) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasNumericContext(
    const CanonicalScalarDatatypeRegistry& registry,
    const Uuid& numeric_context_uuid) noexcept {
  for (const auto& context : registry.numeric_contexts) {
    if (ExecutionDataPacketUuidEquals(context.numeric_context_uuid,
                                      numeric_context_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasResolutionKind(
    const CanonicalScalarDatatypeRegistry& registry,
    CanonicalScalarResolutionKind kind) noexcept {
  for (const auto& rule : registry.literal_bind_rules) {
    if (rule.resolution_kind == kind) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasOperationFamily(
    const CanonicalScalarDatatypeRegistry& registry,
    CanonicalScalarOperationFamily family) noexcept {
  for (const auto& rule : registry.cast_operation_rules) {
    if (rule.operation_family == family) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasTransportProfileForDescriptor(
    const CanonicalScalarDatatypeRegistry& registry,
    const Uuid& descriptor_uuid) noexcept {
  for (const auto& profile : registry.transport_profiles) {
    if (ExecutionDataPacketUuidEquals(profile.descriptor_uuid,
                                      descriptor_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasReferenceMappingForDescriptor(
    const CanonicalScalarDatatypeRegistry& registry,
    const Uuid& descriptor_uuid) noexcept {
  for (const auto& mapping : registry.reference_mappings) {
    if (ExecutionDataPacketUuidEquals(mapping.descriptor_uuid,
                                      descriptor_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalScalarHasPassedGate(
    const CanonicalScalarDatatypeRegistry& registry,
    std::string_view gate_id) noexcept {
  for (const auto& gate : registry.conformance_gates) {
    if (gate.gate_id == gate_id) {
      return gate.status == CanonicalScalarGateStatus::passed &&
             !ExecutionDataPacketUuidIsNil(gate.gate_uuid) &&
             !gate.evidence_hash.empty() && !gate.ctest_name.empty() &&
             !gate.gate_hash.empty();
    }
  }
  return false;
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarNumericContext(const NumericContextRecord& context,
                                      std::size_t context_index) {
  if (ExecutionDataPacketUuidIsNil(context.numeric_context_uuid) ||
      context.context_key.empty() || context.rounding_mode.empty() ||
      context.overflow_policy.empty() || context.underflow_policy.empty() ||
      context.divide_by_zero_policy.empty() || context.nan_policy.empty() ||
      context.infinity_policy.empty() || context.signed_zero_policy.empty() ||
      context.trap_policy.empty() || context.context_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::numeric_context_incomplete, 0,
            context_index};
  }
  if (context.total_order_required && context.total_order_profile.empty()) {
    return {CanonicalScalarDatatypeStatus::numeric_context_incomplete, 0,
            context_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarDescriptor(
    const CanonicalScalarDatatypeRegistry& registry,
    const ScalarTypeDescriptorRecord& descriptor,
    std::size_t descriptor_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.descriptor_uuid)) {
    return {CanonicalScalarDatatypeStatus::descriptor_uuid_required,
            descriptor_index};
  }
  if (descriptor.descriptor_key.empty() ||
      descriptor.descriptor_search_key.empty() ||
      descriptor.descriptor_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::descriptor_identity_incomplete,
            descriptor_index};
  }
  if (!CanonicalScalarTypeIsValid(descriptor.canonical_type)) {
    return {CanonicalScalarDatatypeStatus::descriptor_type_invalid,
            descriptor_index};
  }
  if (!CanonicalScalarFamilyIsValid(descriptor.scalar_family)) {
    return {CanonicalScalarDatatypeStatus::descriptor_family_invalid,
            descriptor_index};
  }
  if (CanonicalScalarTypeFamily(descriptor.canonical_type) !=
      descriptor.scalar_family) {
    return {CanonicalScalarDatatypeStatus::descriptor_family_mismatch,
            descriptor_index};
  }
  if (descriptor.descriptor_search_key !=
      CanonicalScalarTypeSearchKey(descriptor.canonical_type)) {
    return {CanonicalScalarDatatypeStatus::descriptor_identity_incomplete,
            descriptor_index};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.comparison_contract_uuid) ||
      ExecutionDataPacketUuidIsNil(
          descriptor.canonicalization_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.cast_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.operation_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.storage_codec_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.wire_render_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.statistics_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.index_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(
          descriptor.backup_transport_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.diagnostic_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.metric_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(descriptor.conformance_profile_uuid)) {
    return {CanonicalScalarDatatypeStatus::descriptor_policy_missing,
            descriptor_index};
  }
  if (CanonicalScalarTypeRequiresBitWidth(descriptor.canonical_type) &&
      descriptor.bit_width == 0) {
    return {CanonicalScalarDatatypeStatus::fixed_width_required,
            descriptor_index};
  }
  if (CanonicalScalarTypeRequiresPrecision(descriptor.canonical_type) &&
      (!descriptor.precision_declared || descriptor.precision == 0)) {
    return {CanonicalScalarDatatypeStatus::precision_required,
            descriptor_index};
  }
  if (CanonicalScalarTypeRequiresScale(descriptor.canonical_type) &&
      !descriptor.scale_declared) {
    return {CanonicalScalarDatatypeStatus::scale_required, descriptor_index};
  }
  if (descriptor.scale_declared &&
      descriptor.precision_declared &&
      descriptor.scale > descriptor.precision) {
    return {CanonicalScalarDatatypeStatus::scale_invalid, descriptor_index};
  }
  if (!descriptor.value_state_set_complete) {
    return {CanonicalScalarDatatypeStatus::value_state_set_incomplete,
            descriptor_index};
  }
  if (!descriptor.storage_encoding_canonical) {
    return {CanonicalScalarDatatypeStatus::physical_encoding_invalid,
            descriptor_index};
  }
  if (!descriptor.raw_host_encoding_forbidden) {
    return {CanonicalScalarDatatypeStatus::raw_host_encoding_allowed,
            descriptor_index};
  }
  if (CanonicalScalarTypeRequiresNumericContext(descriptor.canonical_type) &&
      (ExecutionDataPacketUuidIsNil(descriptor.numeric_context_uuid) ||
       !CanonicalScalarHasNumericContext(registry,
                                        descriptor.numeric_context_uuid))) {
    return {CanonicalScalarDatatypeStatus::numeric_context_missing,
            descriptor_index};
  }
  if (descriptor.canonical_type == CanonicalScalarType::money_currency &&
      (ExecutionDataPacketUuidIsNil(descriptor.amount_descriptor_uuid) ||
       CanonicalScalarFindDescriptorByUuid(
           registry, descriptor.amount_descriptor_uuid) == nullptr ||
       ExecutionDataPacketUuidIsNil(descriptor.currency_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(
           descriptor.monetary_rounding_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(descriptor.monetary_render_policy_uuid))) {
    return {CanonicalScalarDatatypeStatus::money_currency_policy_missing,
            descriptor_index};
  }
  if (descriptor.canonical_type == CanonicalScalarType::float128) {
    if (descriptor.bit_width != 128 ||
        descriptor.canonical_payload_bytes != 16) {
      return {CanonicalScalarDatatypeStatus::float128_encoding_invalid,
              descriptor_index};
    }
    if (descriptor.portable_binary128_backend_required &&
        !descriptor.portable_binary128_backend_available) {
      return {CanonicalScalarDatatypeStatus::float128_backend_missing,
              descriptor_index};
    }
    if (!descriptor.native_acceleration_guarded ||
        !descriptor.silent_downgrade_forbidden ||
        !descriptor.driver_fallback_disclosed ||
        !descriptor.index_statistics_guarded ||
        !descriptor.float128_operations_complete) {
      return {CanonicalScalarDatatypeStatus::float128_contract_incomplete,
              descriptor_index};
    }
  }
  for (std::size_t other_index = descriptor_index + 1;
       other_index < registry.descriptors.size(); ++other_index) {
    const auto& other = registry.descriptors[other_index];
    if (descriptor.canonical_type == other.canonical_type ||
        descriptor.descriptor_key == other.descriptor_key ||
        ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                      other.descriptor_uuid)) {
      return {CanonicalScalarDatatypeStatus::descriptor_duplicate,
              other_index};
    }
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarLiteralBindRule(
    const ScalarLiteralBindResolutionRule& rule,
    std::size_t rule_index) {
  if (ExecutionDataPacketUuidIsNil(rule.resolution_rule_uuid) ||
      !CanonicalScalarResolutionKindIsValid(rule.resolution_kind) ||
      rule.resolution_search_key !=
          "CSD-SCALAR-LITERAL-BIND-RESOLUTION" ||
      rule.candidate_set_hash.empty() || rule.ranking_rule_hash.empty() ||
      rule.resolved_descriptor_evidence_hash.empty() ||
      rule.ambiguous_diagnostic_code.empty() ||
      rule.no_match_diagnostic_code.empty() || rule.rule_hash.empty() ||
      !rule.engine_resolves_final_descriptor ||
      !rule.driver_metadata_is_hint ||
      !rule.parser_spelling_not_authority) {
    return {CanonicalScalarDatatypeStatus::literal_bind_rule_incomplete, 0, 0,
            rule_index};
  }
  if (!rule.fail_closed_on_ambiguity ||
      rule.ambiguous_diagnostic_code != "SCALAR.AMBIGUOUS_LITERAL" ||
      rule.no_match_diagnostic_code != "SCALAR.INVALID_LITERAL") {
    return {CanonicalScalarDatatypeStatus::
                literal_bind_ambiguity_not_fail_closed,
            0, 0, rule_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarCastOperationRule(
    const CanonicalScalarDatatypeRegistry& registry,
    const ScalarCastOperationRule& rule,
    std::size_t rule_index) {
  if (ExecutionDataPacketUuidIsNil(rule.rule_uuid) ||
      !CanonicalScalarOperationFamilyIsValid(rule.operation_family) ||
      !CanonicalScalarCastRankIsValid(rule.cast_rank) ||
      CanonicalScalarFindDescriptorByUuid(
          registry, rule.source_descriptor_uuid) == nullptr ||
      CanonicalScalarFindDescriptorByUuid(
          registry, rule.target_descriptor_uuid) == nullptr ||
      CanonicalScalarFindDescriptorByUuid(
          registry, rule.result_descriptor_uuid) == nullptr ||
      rule.operation_search_key.empty() || rule.operation_policy_hash.empty() ||
      rule.failure_diagnostic_code.empty() || rule.rule_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::cast_operation_rule_incomplete, 0,
            0, rule_index};
  }
  if (!rule.range_check_declared ||
      !rule.precision_scale_check_declared ||
      !rule.special_value_policy_checked ||
      !rule.currency_policy_checked ||
      !rule.security_policy_checked ||
      !rule.reference_profile_checked ||
      !rule.result_descriptor_declared || !rule.no_silent_fallback) {
    return {CanonicalScalarDatatypeStatus::cast_operation_checks_incomplete,
            0, 0, rule_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarTransportProfile(
    const CanonicalScalarDatatypeRegistry& registry,
    const ScalarTransportProfileRecord& profile,
    std::size_t rule_index) {
  if (ExecutionDataPacketUuidIsNil(profile.transport_profile_uuid) ||
      CanonicalScalarFindDescriptorByUuid(registry,
                                         profile.descriptor_uuid) == nullptr ||
      profile.transport_search_key !=
          "CSD-SCALAR-BACKUP-REPLICATION-TRANSPORT" ||
      !profile.backup_manifest_records_descriptor ||
      !profile.restore_incompatible_refuses ||
      !profile.replication_descriptor_version_checked ||
      !profile.cluster_transport_negotiates_descriptor_codec ||
      !profile.merge_uses_canonical_equality ||
      !profile.driver_metadata_discloses_limitations ||
      profile.transport_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::transport_profile_incomplete, 0,
            0, rule_index};
  }
  if (!profile.logical_delta_derivative_only) {
    return {CanonicalScalarDatatypeStatus::
                transport_recovery_authority_violation,
            0, 0, rule_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarReferenceMapping(
    const CanonicalScalarDatatypeRegistry& registry,
    const ScalarReferenceMappingRecord& mapping,
    std::size_t rule_index) {
  if (ExecutionDataPacketUuidIsNil(mapping.reference_mapping_uuid) ||
      CanonicalScalarFindDescriptorByUuid(registry,
                                         mapping.descriptor_uuid) == nullptr ||
      mapping.reference_profile_key.empty() || mapping.reference_type_name.empty() ||
      !CanonicalScalarReferenceBehaviorIsValid(mapping.behavior) ||
      mapping.diagnostic_code.empty() || mapping.mapping_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::reference_mapping_incomplete, 0, 0,
            rule_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarConformanceGate(
    const CanonicalScalarConformanceGateRecord& gate,
    std::size_t gate_index) {
  if (ExecutionDataPacketUuidIsNil(gate.gate_uuid) ||
      gate.gate_id.empty() || !CanonicalScalarGateStatusIsValid(gate.status) ||
      gate.evidence_hash.empty() || gate.ctest_name.empty() ||
      gate.gate_hash.empty()) {
    return {CanonicalScalarDatatypeStatus::conformance_gate_missing, 0, 0, 0,
            gate_index};
  }
  if (gate.status != CanonicalScalarGateStatus::passed) {
    return {CanonicalScalarDatatypeStatus::conformance_gate_failed, 0, 0, 0,
            gate_index};
  }
  return {};
}

inline CanonicalScalarDatatypeValidationResult
ValidateCanonicalScalarDatatypeRegistry(
    const CanonicalScalarDatatypeRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {CanonicalScalarDatatypeStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {CanonicalScalarDatatypeStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {CanonicalScalarDatatypeStatus::registry_name_required};
  }
  if (registry.root_search_key != "CSD-CANONICAL-SCALAR-DATATYPES") {
    return {CanonicalScalarDatatypeStatus::root_search_key_required};
  }
  if (registry.descriptors.empty()) {
    return {CanonicalScalarDatatypeStatus::descriptor_records_required};
  }
  if (registry.numeric_contexts.empty()) {
    return {CanonicalScalarDatatypeStatus::numeric_context_records_required};
  }
  if (registry.literal_bind_rules.empty()) {
    return {CanonicalScalarDatatypeStatus::literal_bind_rules_required};
  }
  if (registry.cast_operation_rules.empty()) {
    return {CanonicalScalarDatatypeStatus::cast_operation_rules_required};
  }
  if (registry.transport_profiles.empty()) {
    return {CanonicalScalarDatatypeStatus::transport_profiles_required};
  }
  if (registry.reference_mappings.empty()) {
    return {CanonicalScalarDatatypeStatus::reference_mapping_records_required};
  }
  if (registry.conformance_gates.empty()) {
    return {CanonicalScalarDatatypeStatus::conformance_gate_records_required};
  }
  if (!registry.descriptor_authority_preserved ||
      !registry.parser_sblr_boundary_preserved ||
      !registry.reference_names_not_authority ||
      !registry.storage_wire_representation_not_authority ||
      !registry.driver_metadata_hint_only ||
      !registry.mga_authority_preserved ||
      !registry.write_after_delta_not_recovery_authority ||
      !registry.silent_float128_downgrade_forbidden) {
    return {CanonicalScalarDatatypeStatus::authority_invariant_violation};
  }
  constexpr CanonicalScalarType required_types[] = {
      CanonicalScalarType::boolean,
      CanonicalScalarType::int8,
      CanonicalScalarType::int16,
      CanonicalScalarType::int32,
      CanonicalScalarType::int64,
      CanonicalScalarType::int128,
      CanonicalScalarType::uint8,
      CanonicalScalarType::uint16,
      CanonicalScalarType::uint32,
      CanonicalScalarType::uint64,
      CanonicalScalarType::uint128,
      CanonicalScalarType::exact_numeric,
      CanonicalScalarType::decimal_floating,
      CanonicalScalarType::float32,
      CanonicalScalarType::float64,
      CanonicalScalarType::float128,
      CanonicalScalarType::money_currency};
  for (const auto type : required_types) {
    if (!CanonicalScalarHasDescriptorType(registry, type)) {
      return {CanonicalScalarDatatypeStatus::descriptor_record_missing};
    }
  }
  for (std::size_t context_index = 0;
       context_index < registry.numeric_contexts.size(); ++context_index) {
    const auto context_result = ValidateCanonicalScalarNumericContext(
        registry.numeric_contexts[context_index], context_index);
    if (!context_result.ok()) {
      return context_result;
    }
  }
  for (std::size_t descriptor_index = 0;
       descriptor_index < registry.descriptors.size(); ++descriptor_index) {
    const auto descriptor_result = ValidateCanonicalScalarDescriptor(
        registry, registry.descriptors[descriptor_index], descriptor_index);
    if (!descriptor_result.ok()) {
      return descriptor_result;
    }
  }
  constexpr CanonicalScalarResolutionKind required_resolution_kinds[] = {
      CanonicalScalarResolutionKind::literal,
      CanonicalScalarResolutionKind::bind_parameter};
  for (const auto kind : required_resolution_kinds) {
    if (!CanonicalScalarHasResolutionKind(registry, kind)) {
      return {CanonicalScalarDatatypeStatus::literal_bind_rule_missing};
    }
  }
  for (std::size_t rule_index = 0;
       rule_index < registry.literal_bind_rules.size(); ++rule_index) {
    const auto rule_result = ValidateCanonicalScalarLiteralBindRule(
        registry.literal_bind_rules[rule_index], rule_index);
    if (!rule_result.ok()) {
      return rule_result;
    }
  }
  constexpr CanonicalScalarOperationFamily required_operation_families[] = {
      CanonicalScalarOperationFamily::cast,
      CanonicalScalarOperationFamily::equality_distinctness,
      CanonicalScalarOperationFamily::ordering,
      CanonicalScalarOperationFamily::hashing,
      CanonicalScalarOperationFamily::arithmetic,
      CanonicalScalarOperationFamily::aggregate,
      CanonicalScalarOperationFamily::window,
      CanonicalScalarOperationFamily::serialization};
  for (const auto family : required_operation_families) {
    if (!CanonicalScalarHasOperationFamily(registry, family)) {
      return {CanonicalScalarDatatypeStatus::cast_operation_rule_missing};
    }
  }
  for (std::size_t rule_index = 0;
       rule_index < registry.cast_operation_rules.size(); ++rule_index) {
    const auto rule_result = ValidateCanonicalScalarCastOperationRule(
        registry, registry.cast_operation_rules[rule_index], rule_index);
    if (!rule_result.ok()) {
      return rule_result;
    }
  }
  for (const auto& descriptor : registry.descriptors) {
    if (!CanonicalScalarHasTransportProfileForDescriptor(
            registry, descriptor.descriptor_uuid)) {
      return {CanonicalScalarDatatypeStatus::transport_profile_missing};
    }
    if (!CanonicalScalarHasReferenceMappingForDescriptor(
            registry, descriptor.descriptor_uuid)) {
      return {CanonicalScalarDatatypeStatus::reference_mapping_missing};
    }
  }
  for (std::size_t rule_index = 0;
       rule_index < registry.transport_profiles.size(); ++rule_index) {
    const auto profile_result = ValidateCanonicalScalarTransportProfile(
        registry, registry.transport_profiles[rule_index], rule_index);
    if (!profile_result.ok()) {
      return profile_result;
    }
  }
  for (std::size_t rule_index = 0;
       rule_index < registry.reference_mappings.size(); ++rule_index) {
    const auto mapping_result = ValidateCanonicalScalarReferenceMapping(
        registry, registry.reference_mappings[rule_index], rule_index);
    if (!mapping_result.ok()) {
      return mapping_result;
    }
  }
  for (std::size_t gate_index = 0;
       gate_index < registry.conformance_gates.size(); ++gate_index) {
    const auto gate_result = ValidateCanonicalScalarConformanceGate(
        registry.conformance_gates[gate_index], gate_index);
    if (!gate_result.ok()) {
      return gate_result;
    }
  }
  constexpr std::string_view required_gates[] = {
      "CSD-GATE-001",
      "CSD-GATE-002",
      "CSD-GATE-003",
      "CSD-GATE-004",
      "CSD-GATE-005",
      "CSD-GATE-006",
      "CSD-GATE-007",
      "CSD-GATE-008",
      "CSD-GATE-009",
      "CSD-GATE-010",
      "CSD-GATE-011",
      "SCALAR-CONF-001",
      "SCALAR-CONF-002",
      "SCALAR-CONF-003",
      "SCALAR-CONF-004",
      "SCALAR-CONF-005",
      "SCALAR-CONF-006",
      "SCALAR-CONF-007",
      "SCALAR-CONF-008",
      "SCALAR-CONF-009",
      "SCALAR-CONF-010"};
  for (std::string_view gate_id : required_gates) {
    if (!CanonicalScalarHasPassedGate(registry, gate_id)) {
      return {CanonicalScalarDatatypeStatus::conformance_gate_missing};
    }
  }
  if (!registry.diagnostics_root_declared) {
    return {CanonicalScalarDatatypeStatus::diagnostic_vector_missing};
  }
  constexpr std::string_view required_diagnostics[] = {
      "SCALAR.DESCRIPTOR.INVALID",
      "SCALAR.INVALID_LITERAL",
      "SCALAR.AMBIGUOUS_LITERAL",
      "SCALAR.AMBIGUOUS_CAST",
      "SCALAR.OUT_OF_RANGE",
      "SCALAR.PRECISION_LOSS",
      "SCALAR.SCALE_LOSS",
      "SCALAR.OVERFLOW",
      "SCALAR.UNDERFLOW",
      "SCALAR.DIVIDE_BY_ZERO",
      "SCALAR.SPECIAL_VALUE_REFUSED",
      "SCALAR.CURRENCY_MISMATCH",
      "SCALAR.BACKEND_UNAVAILABLE",
      "SCALAR.RAW_HOST_ENCODING_REFUSED",
      "SCALAR.REFERENCE.MAPPING_MISSING",
      "SCALAR.TRANSPORT.UNSUPPORTED",
      "SCALAR.MERGE.MANUAL_REVIEW_REQUIRED",
      "float128_backend_unavailable",
      "float128_raw_host_encoding_refused"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!CanonicalScalarHasDiagnosticCode(registry, diagnostic_code)) {
      return {CanonicalScalarDatatypeStatus::diagnostic_vector_missing};
    }
  }
  if (!registry.local_metrics_root_declared) {
    return {CanonicalScalarDatatypeStatus::local_metric_missing};
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.datatypes.scalar.descriptor.admissions_total",
      "sys.metrics.datatypes.scalar.literal.invalid_total",
      "sys.metrics.datatypes.scalar.literal.ambiguous_total",
      "sys.metrics.datatypes.scalar.cast.attempts_total",
      "sys.metrics.datatypes.scalar.cast.failures_total",
      "sys.metrics.datatypes.scalar.operation.overflow_total",
      "sys.metrics.datatypes.scalar.operation.underflow_total",
      "sys.metrics.datatypes.scalar.operation.divide_by_zero_total",
      "sys.metrics.datatypes.scalar.operation.precision_loss_refusals_total",
      "sys.metrics.datatypes.scalar.operation.scale_loss_refusals_total",
      "sys.metrics.datatypes.scalar.numeric_context.special_value_refusals_total",
      "sys.metrics.datatypes.scalar.float128.backend_fallbacks_total",
      "sys.metrics.datatypes.scalar.float128.backend_unavailable_total",
      "sys.metrics.datatypes.scalar.float128.raw_host_encoding_refusals_total",
      "sys.metrics.datatypes.scalar.money.currency_mismatches_total",
      "sys.metrics.datatypes.scalar.transport.refusals_total",
      "sys.metrics.datatypes.scalar.reference.mapping_misses_total",
      "sys.metrics.datatypes.scalar.merge.manual_review_required_total"};
  for (std::string_view metric : required_metrics) {
    if (!CanonicalScalarHasMetric(registry, metric)) {
      return {CanonicalScalarDatatypeStatus::local_metric_missing};
    }
  }
  if (!registry.cluster_metrics_guarded_by_cluster_governance) {
    return {CanonicalScalarDatatypeStatus::cluster_metric_guard_required};
  }
  return {};
}

// SEARCH_KEY: NVD-NATIVE-VS-DOMAIN-DATATYPE-DECISIONS
// SEARCH_KEY: NVD-CORE-DECISION
// SEARCH_KEY: NVD-NATIVE-PROMOTION-CRITERIA
// SEARCH_KEY: NVD-DOMAIN-ONLY-CRITERIA
// SEARCH_KEY: NVD-REQUIRED-NATIVE-SUBSTRATE-FAMILIES
// SEARCH_KEY: NVD-DOMAIN-ONLY-FAMILIES
// SEARCH_KEY: NVD-OPAQUE-UDR-BRIDGE-FAMILIES
// SEARCH_KEY: NVD-REFERENCE-FAMILY-DECISIONS
// SEARCH_KEY: NVD-FULL-SUPPORT-RULE
enum class NativeDomainDecisionClass : std::uint8_t {
  native_substrate = 0,
  domain_over_native = 1,
  compound_domain = 2,
  opaque_domain = 3,
  locator_domain = 4,
  cxx_udr_bridge = 5,
  render_only_metadata = 6,
  version_deferred = 7,
  policy_unsupported = 8
};

constexpr bool NativeDomainDecisionClassIsValid(
    NativeDomainDecisionClass decision_class) noexcept {
  switch (decision_class) {
    case NativeDomainDecisionClass::native_substrate:
    case NativeDomainDecisionClass::domain_over_native:
    case NativeDomainDecisionClass::compound_domain:
    case NativeDomainDecisionClass::opaque_domain:
    case NativeDomainDecisionClass::locator_domain:
    case NativeDomainDecisionClass::cxx_udr_bridge:
    case NativeDomainDecisionClass::render_only_metadata:
    case NativeDomainDecisionClass::version_deferred:
    case NativeDomainDecisionClass::policy_unsupported:
      return true;
  }
  return false;
}

constexpr bool NativeDomainDecisionClassIsUnsupportedOrDeferred(
    NativeDomainDecisionClass decision_class) noexcept {
  return decision_class == NativeDomainDecisionClass::version_deferred ||
         decision_class == NativeDomainDecisionClass::policy_unsupported;
}

constexpr bool NativeDomainDecisionClassRequiresNativeSubstrate(
    NativeDomainDecisionClass decision_class) noexcept {
  return !NativeDomainDecisionClassIsUnsupportedOrDeferred(decision_class) &&
         decision_class != NativeDomainDecisionClass::render_only_metadata;
}

enum class NativeDomainReferenceFamily : std::uint8_t {
  postgresql = 0,
  firebird = 1,
  mysql_mariadb = 2,
  oracle = 3,
  sql_server = 4,
  db2 = 5,
  sqlite = 6,
  cassandra = 7,
  mongodb = 8,
  clickhouse = 9,
  duckdb = 10,
  opensearch = 11,
  redis = 12,
  neo4j = 13,
  influxdb = 14,
  milvus = 15,
  non_reference = 16
};

constexpr bool NativeDomainReferenceFamilyIsValid(
    NativeDomainReferenceFamily family) noexcept {
  switch (family) {
    case NativeDomainReferenceFamily::postgresql:
    case NativeDomainReferenceFamily::firebird:
    case NativeDomainReferenceFamily::mysql_mariadb:
    case NativeDomainReferenceFamily::oracle:
    case NativeDomainReferenceFamily::sql_server:
    case NativeDomainReferenceFamily::db2:
    case NativeDomainReferenceFamily::sqlite:
    case NativeDomainReferenceFamily::cassandra:
    case NativeDomainReferenceFamily::mongodb:
    case NativeDomainReferenceFamily::clickhouse:
    case NativeDomainReferenceFamily::duckdb:
    case NativeDomainReferenceFamily::opensearch:
    case NativeDomainReferenceFamily::redis:
    case NativeDomainReferenceFamily::neo4j:
    case NativeDomainReferenceFamily::influxdb:
    case NativeDomainReferenceFamily::milvus:
    case NativeDomainReferenceFamily::non_reference:
      return true;
  }
  return false;
}

enum class NativeDomainReferenceTypeCategory : std::uint8_t {
  scalar = 0,
  compound = 1,
  collection = 2,
  document = 3,
  spatial = 4,
  vector = 5,
  graph = 6,
  timeseries = 7,
  locator = 8,
  pseudo = 9,
  opaque = 10,
  extension = 11,
  unsupported = 12
};

constexpr bool NativeDomainReferenceTypeCategoryIsValid(
    NativeDomainReferenceTypeCategory category) noexcept {
  switch (category) {
    case NativeDomainReferenceTypeCategory::scalar:
    case NativeDomainReferenceTypeCategory::compound:
    case NativeDomainReferenceTypeCategory::collection:
    case NativeDomainReferenceTypeCategory::document:
    case NativeDomainReferenceTypeCategory::spatial:
    case NativeDomainReferenceTypeCategory::vector:
    case NativeDomainReferenceTypeCategory::graph:
    case NativeDomainReferenceTypeCategory::timeseries:
    case NativeDomainReferenceTypeCategory::locator:
    case NativeDomainReferenceTypeCategory::pseudo:
    case NativeDomainReferenceTypeCategory::opaque:
    case NativeDomainReferenceTypeCategory::extension:
    case NativeDomainReferenceTypeCategory::unsupported:
      return true;
  }
  return false;
}

enum class NativeDomainReferenceCoverageState : std::uint8_t {
  none = 0,
  partial = 1,
  complete_for_claimed_references = 2,
  complete_for_all_targeted_references = 3
};

constexpr bool NativeDomainReferenceCoverageStateIsValid(
    NativeDomainReferenceCoverageState state) noexcept {
  switch (state) {
    case NativeDomainReferenceCoverageState::none:
    case NativeDomainReferenceCoverageState::partial:
    case NativeDomainReferenceCoverageState::complete_for_claimed_references:
    case NativeDomainReferenceCoverageState::complete_for_all_targeted_references:
      return true;
  }
  return false;
}

enum class NativeDomainConformanceStatus : std::uint8_t {
  not_started = 0,
  specified = 1,
  implementation_ready = 2,
  implemented = 3,
  conformance_passed = 4,
  deferred = 5,
  unsupported = 6
};

constexpr bool NativeDomainConformanceStatusIsValid(
    NativeDomainConformanceStatus status) noexcept {
  switch (status) {
    case NativeDomainConformanceStatus::not_started:
    case NativeDomainConformanceStatus::specified:
    case NativeDomainConformanceStatus::implementation_ready:
    case NativeDomainConformanceStatus::implemented:
    case NativeDomainConformanceStatus::conformance_passed:
    case NativeDomainConformanceStatus::deferred:
    case NativeDomainConformanceStatus::unsupported:
      return true;
  }
  return false;
}

constexpr bool NativeDomainConformanceStatusIsImplementationReady(
    NativeDomainConformanceStatus status) noexcept {
  return status == NativeDomainConformanceStatus::implementation_ready ||
         status == NativeDomainConformanceStatus::implemented ||
         status == NativeDomainConformanceStatus::conformance_passed;
}

enum class NativeDomainUnsupportedBehavior : std::uint8_t {
  none = 0,
  unsupported_by_version = 1,
  unsupported_by_policy = 2,
  unsupported_by_profile = 3,
  requires_cxx_udr = 4,
  requires_license = 5
};

constexpr bool NativeDomainUnsupportedBehaviorIsValid(
    NativeDomainUnsupportedBehavior behavior) noexcept {
  switch (behavior) {
    case NativeDomainUnsupportedBehavior::none:
    case NativeDomainUnsupportedBehavior::unsupported_by_version:
    case NativeDomainUnsupportedBehavior::unsupported_by_policy:
    case NativeDomainUnsupportedBehavior::unsupported_by_profile:
    case NativeDomainUnsupportedBehavior::requires_cxx_udr:
    case NativeDomainUnsupportedBehavior::requires_license:
      return true;
  }
  return false;
}

enum class NativeDomainGateStatus : std::uint8_t {
  passed = 0,
  failed = 1,
  deferred = 2,
  unsupported = 3
};

constexpr bool NativeDomainGateStatusIsValid(
    NativeDomainGateStatus status) noexcept {
  switch (status) {
    case NativeDomainGateStatus::passed:
    case NativeDomainGateStatus::failed:
    case NativeDomainGateStatus::deferred:
    case NativeDomainGateStatus::unsupported:
      return true;
  }
  return false;
}

struct DatatypeDecisionRecord {
  Uuid decision_uuid{};
  std::string scratchbird_family_name;
  NativeDomainDecisionClass decision_class =
      NativeDomainDecisionClass::native_substrate;
  std::string native_substrate_family;
  Uuid base_physical_encoding_uuid{};
  Uuid in_page_representation_uuid{};
  Uuid out_of_page_representation_uuid{};
  bool out_of_page_possible = false;
  Uuid comparison_profile_uuid{};
  Uuid casting_profile_uuid{};
  Uuid operation_profile_uuid{};
  Uuid statistics_profile_uuid{};
  Uuid index_profile_uuid{};
  Uuid driver_metadata_profile_uuid{};
  Uuid backup_restore_profile_uuid{};
  Uuid transport_profile_uuid{};
  Uuid diagnostic_profile_uuid{};
  NativeDomainReferenceCoverageState reference_coverage_state =
      NativeDomainReferenceCoverageState::none;
  Uuid cxx_udr_package_uuid{};
  std::string unsupported_reason;
  NativeDomainConformanceStatus conformance_status =
      NativeDomainConformanceStatus::specified;
  std::string controlling_search_key;
  bool native_promotion_criteria_satisfied = false;
  bool domain_only_justification_present = false;
  bool compound_element_addressing_present = false;
  bool opaque_lifecycle_policy_present = false;
  bool locator_authority_policy_present = false;
  bool render_only_execution_forbidden = false;
  bool non_cpp_udr_forbidden = true;
  bool parser_name_not_identity = true;
  bool implementation_trace_anchor_present = false;
  std::string decision_hash;
};

struct ReferenceDatatypeMappingRecord {
  Uuid mapping_uuid{};
  NativeDomainReferenceFamily reference_family = NativeDomainReferenceFamily::non_reference;
  std::string reference_version_range;
  std::string reference_type_name;
  NativeDomainReferenceTypeCategory reference_type_category =
      NativeDomainReferenceTypeCategory::scalar;
  Uuid scratchbird_decision_uuid{};
  Uuid literal_policy_uuid{};
  Uuid bind_policy_uuid{};
  Uuid cast_policy_uuid{};
  Uuid operation_policy_uuid{};
  Uuid index_statistics_policy_uuid{};
  Uuid storage_policy_uuid{};
  Uuid transport_policy_uuid{};
  Uuid diagnostic_policy_uuid{};
  NativeDomainUnsupportedBehavior unsupported_behavior =
      NativeDomainUnsupportedBehavior::none;
  std::string conformance_test_id;
  bool parser_accepts_type = true;
  bool full_support_claimed = false;
  std::string mapping_hash;
};

struct CompoundDomainElementRecord {
  Uuid element_uuid{};
  Uuid decision_uuid{};
  std::string element_name;
  std::uint32_t element_ordinal = 0;
  std::string path_expression;
  std::string nullability_policy;
  Uuid cast_rule_uuid{};
  Uuid operation_rule_uuid{};
  std::string versioning_rule;
  bool parser_name_not_identity = true;
  std::string element_hash;
};

struct ReferenceFamilyFullSupportClaimRecord {
  Uuid claim_uuid{};
  NativeDomainReferenceFamily reference_family = NativeDomainReferenceFamily::non_reference;
  std::uint32_t expected_type_row_count = 0;
  std::uint32_t mapped_type_row_count = 0;
  std::string conformance_manifest_hash;
  bool full_support_claimed = false;
  std::string claim_hash;
};

struct NativeDomainConformanceGateRecord {
  Uuid gate_uuid{};
  std::string gate_id;
  NativeDomainGateStatus status = NativeDomainGateStatus::failed;
  std::string evidence_hash;
  std::string ctest_name;
  std::string diagnostic_code;
  std::string gate_hash;
};

struct NativeDomainDatatypeDecisionRegistry {
  Uuid registry_uuid{};
  std::uint64_t registry_epoch = 0;
  std::string registry_name;
  std::string root_search_key;
  std::vector<DatatypeDecisionRecord> decisions;
  std::vector<ReferenceDatatypeMappingRecord> reference_mappings;
  std::vector<CompoundDomainElementRecord> compound_elements;
  std::vector<ReferenceFamilyFullSupportClaimRecord> full_support_claims;
  std::vector<NativeDomainConformanceGateRecord> conformance_gates;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool descriptor_authority_preserved = true;
  bool uuid_identity_preserved = true;
  bool parser_sblr_boundary_preserved = true;
  bool reference_compatibility_not_authority = true;
  bool non_cpp_udr_forbidden = true;
  bool unsupported_diagnostics_required = true;
  bool full_support_requires_complete_mappings = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
};

enum class NativeDomainDatatypeDecisionStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  root_search_key_required = 4,
  decision_records_required = 5,
  reference_mapping_records_required = 6,
  conformance_gate_records_required = 7,
  authority_invariant_violation = 8,
  decision_record_missing = 9,
  decision_uuid_required = 10,
  decision_identity_incomplete = 11,
  decision_class_invalid = 12,
  reference_coverage_state_invalid = 13,
  conformance_status_invalid = 14,
  native_substrate_missing = 15,
  descriptor_missing = 16,
  native_promotion_missing = 17,
  domain_justification_missing = 18,
  compound_element_missing = 19,
  compound_element_invalid = 20,
  opaque_lifecycle_missing = 21,
  locator_policy_missing = 22,
  cxx_udr_package_missing = 23,
  render_only_policy_missing = 24,
  unsupported_reason_missing = 25,
  non_cpp_udr_allowed = 26,
  decision_duplicate = 27,
  reference_mapping_missing = 28,
  reference_mapping_incomplete = 29,
  reference_mapping_orphan = 30,
  reference_mapping_policy_missing = 31,
  unsupported_behavior_missing = 32,
  full_support_overclaim = 33,
  conformance_gate_missing = 34,
  conformance_gate_failed = 35,
  diagnostic_vector_missing = 36,
  local_metric_missing = 37,
  cluster_metric_guard_required = 38
};

constexpr std::string_view NativeDomainDatatypeDecisionStatusName(
    NativeDomainDatatypeDecisionStatus status) noexcept {
  switch (status) {
    case NativeDomainDatatypeDecisionStatus::ok:
      return "ok";
    case NativeDomainDatatypeDecisionStatus::registry_uuid_required:
      return "registry_uuid_required";
    case NativeDomainDatatypeDecisionStatus::registry_epoch_required:
      return "registry_epoch_required";
    case NativeDomainDatatypeDecisionStatus::registry_name_required:
      return "registry_name_required";
    case NativeDomainDatatypeDecisionStatus::root_search_key_required:
      return "root_search_key_required";
    case NativeDomainDatatypeDecisionStatus::decision_records_required:
      return "decision_records_required";
    case NativeDomainDatatypeDecisionStatus::reference_mapping_records_required:
      return "reference_mapping_records_required";
    case NativeDomainDatatypeDecisionStatus::conformance_gate_records_required:
      return "conformance_gate_records_required";
    case NativeDomainDatatypeDecisionStatus::authority_invariant_violation:
      return "authority_invariant_violation";
    case NativeDomainDatatypeDecisionStatus::decision_record_missing:
      return "decision_record_missing";
    case NativeDomainDatatypeDecisionStatus::decision_uuid_required:
      return "decision_uuid_required";
    case NativeDomainDatatypeDecisionStatus::decision_identity_incomplete:
      return "decision_identity_incomplete";
    case NativeDomainDatatypeDecisionStatus::decision_class_invalid:
      return "decision_class_invalid";
    case NativeDomainDatatypeDecisionStatus::reference_coverage_state_invalid:
      return "reference_coverage_state_invalid";
    case NativeDomainDatatypeDecisionStatus::conformance_status_invalid:
      return "conformance_status_invalid";
    case NativeDomainDatatypeDecisionStatus::native_substrate_missing:
      return "native_substrate_missing";
    case NativeDomainDatatypeDecisionStatus::descriptor_missing:
      return "descriptor_missing";
    case NativeDomainDatatypeDecisionStatus::native_promotion_missing:
      return "native_promotion_missing";
    case NativeDomainDatatypeDecisionStatus::domain_justification_missing:
      return "domain_justification_missing";
    case NativeDomainDatatypeDecisionStatus::compound_element_missing:
      return "compound_element_missing";
    case NativeDomainDatatypeDecisionStatus::compound_element_invalid:
      return "compound_element_invalid";
    case NativeDomainDatatypeDecisionStatus::opaque_lifecycle_missing:
      return "opaque_lifecycle_missing";
    case NativeDomainDatatypeDecisionStatus::locator_policy_missing:
      return "locator_policy_missing";
    case NativeDomainDatatypeDecisionStatus::cxx_udr_package_missing:
      return "cxx_udr_package_missing";
    case NativeDomainDatatypeDecisionStatus::render_only_policy_missing:
      return "render_only_policy_missing";
    case NativeDomainDatatypeDecisionStatus::unsupported_reason_missing:
      return "unsupported_reason_missing";
    case NativeDomainDatatypeDecisionStatus::non_cpp_udr_allowed:
      return "non_cpp_udr_allowed";
    case NativeDomainDatatypeDecisionStatus::decision_duplicate:
      return "decision_duplicate";
    case NativeDomainDatatypeDecisionStatus::reference_mapping_missing:
      return "reference_mapping_missing";
    case NativeDomainDatatypeDecisionStatus::reference_mapping_incomplete:
      return "reference_mapping_incomplete";
    case NativeDomainDatatypeDecisionStatus::reference_mapping_orphan:
      return "reference_mapping_orphan";
    case NativeDomainDatatypeDecisionStatus::reference_mapping_policy_missing:
      return "reference_mapping_policy_missing";
    case NativeDomainDatatypeDecisionStatus::unsupported_behavior_missing:
      return "unsupported_behavior_missing";
    case NativeDomainDatatypeDecisionStatus::full_support_overclaim:
      return "full_support_overclaim";
    case NativeDomainDatatypeDecisionStatus::conformance_gate_missing:
      return "conformance_gate_missing";
    case NativeDomainDatatypeDecisionStatus::conformance_gate_failed:
      return "conformance_gate_failed";
    case NativeDomainDatatypeDecisionStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case NativeDomainDatatypeDecisionStatus::local_metric_missing:
      return "local_metric_missing";
    case NativeDomainDatatypeDecisionStatus::cluster_metric_guard_required:
      return "cluster_metric_guard_required";
  }
  return "unknown";
}

struct NativeDomainDatatypeDecisionValidationResult {
  NativeDomainDatatypeDecisionStatus status =
      NativeDomainDatatypeDecisionStatus::ok;
  std::size_t decision_index = 0;
  std::size_t mapping_index = 0;
  std::size_t gate_index = 0;

  bool ok() const noexcept {
    return status == NativeDomainDatatypeDecisionStatus::ok;
  }
};

inline bool NativeDomainHasDiagnosticCode(
    const NativeDomainDatatypeDecisionRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

inline bool NativeDomainHasMetric(
    const NativeDomainDatatypeDecisionRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline const DatatypeDecisionRecord* NativeDomainFindDecisionByUuid(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const Uuid& decision_uuid) noexcept {
  for (const auto& decision : registry.decisions) {
    if (ExecutionDataPacketUuidEquals(decision.decision_uuid,
                                      decision_uuid)) {
      return &decision;
    }
  }
  return nullptr;
}

inline bool NativeDomainHasDecisionFamily(
    const NativeDomainDatatypeDecisionRegistry& registry,
    std::string_view family_name) noexcept {
  for (const auto& decision : registry.decisions) {
    if (decision.scratchbird_family_name == family_name) {
      return true;
    }
  }
  return false;
}

inline bool NativeDomainHasDecisionClass(
    const NativeDomainDatatypeDecisionRegistry& registry,
    NativeDomainDecisionClass decision_class) noexcept {
  for (const auto& decision : registry.decisions) {
    if (decision.decision_class == decision_class) {
      return true;
    }
  }
  return false;
}

inline bool NativeDomainHasMappingForDecision(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const Uuid& decision_uuid) noexcept {
  for (const auto& mapping : registry.reference_mappings) {
    if (ExecutionDataPacketUuidEquals(mapping.scratchbird_decision_uuid,
                                      decision_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool NativeDomainHasCompoundElementForDecision(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const Uuid& decision_uuid) noexcept {
  for (const auto& element : registry.compound_elements) {
    if (ExecutionDataPacketUuidEquals(element.decision_uuid,
                                      decision_uuid)) {
      return true;
    }
  }
  return false;
}

inline bool NativeDomainHasPassedGate(
    const NativeDomainDatatypeDecisionRegistry& registry,
    std::string_view gate_id) noexcept {
  for (const auto& gate : registry.conformance_gates) {
    if (gate.gate_id == gate_id) {
      return gate.status == NativeDomainGateStatus::passed &&
             !ExecutionDataPacketUuidIsNil(gate.gate_uuid) &&
             !gate.evidence_hash.empty() && !gate.ctest_name.empty() &&
             !gate.gate_hash.empty();
    }
  }
  return false;
}

inline bool NativeDomainDecisionHasImplementationDescriptors(
    const DatatypeDecisionRecord& decision) noexcept {
  if (ExecutionDataPacketUuidIsNil(decision.base_physical_encoding_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.in_page_representation_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.comparison_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.casting_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.operation_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.statistics_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.index_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.driver_metadata_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.backup_restore_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.transport_profile_uuid) ||
      ExecutionDataPacketUuidIsNil(decision.diagnostic_profile_uuid)) {
    return false;
  }
  if (decision.out_of_page_possible &&
      ExecutionDataPacketUuidIsNil(decision.out_of_page_representation_uuid)) {
    return false;
  }
  return true;
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainDecision(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const DatatypeDecisionRecord& decision,
    std::size_t decision_index) {
  if (ExecutionDataPacketUuidIsNil(decision.decision_uuid)) {
    return {NativeDomainDatatypeDecisionStatus::decision_uuid_required,
            decision_index};
  }
  if (decision.scratchbird_family_name.empty() ||
      decision.controlling_search_key.empty() || decision.decision_hash.empty()) {
    return {NativeDomainDatatypeDecisionStatus::decision_identity_incomplete,
            decision_index};
  }
  if (!NativeDomainDecisionClassIsValid(decision.decision_class)) {
    return {NativeDomainDatatypeDecisionStatus::decision_class_invalid,
            decision_index};
  }
  if (!NativeDomainReferenceCoverageStateIsValid(
          decision.reference_coverage_state)) {
    return {NativeDomainDatatypeDecisionStatus::reference_coverage_state_invalid,
            decision_index};
  }
  if (!NativeDomainConformanceStatusIsValid(
          decision.conformance_status)) {
    return {NativeDomainDatatypeDecisionStatus::conformance_status_invalid,
            decision_index};
  }
  if (NativeDomainDecisionClassRequiresNativeSubstrate(
          decision.decision_class) &&
      decision.native_substrate_family.empty()) {
    return {NativeDomainDatatypeDecisionStatus::native_substrate_missing,
            decision_index};
  }
  if (NativeDomainConformanceStatusIsImplementationReady(
          decision.conformance_status) &&
      !NativeDomainDecisionClassIsUnsupportedOrDeferred(
          decision.decision_class) &&
      !NativeDomainDecisionHasImplementationDescriptors(decision)) {
    return {NativeDomainDatatypeDecisionStatus::descriptor_missing,
            decision_index};
  }
  switch (decision.decision_class) {
    case NativeDomainDecisionClass::native_substrate:
      if (!decision.native_promotion_criteria_satisfied) {
        return {NativeDomainDatatypeDecisionStatus::native_promotion_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::domain_over_native:
      if (!decision.domain_only_justification_present) {
        return {NativeDomainDatatypeDecisionStatus::domain_justification_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::compound_domain:
      if (!decision.compound_element_addressing_present ||
          !NativeDomainHasCompoundElementForDecision(registry,
                                                    decision.decision_uuid)) {
        return {NativeDomainDatatypeDecisionStatus::compound_element_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::opaque_domain:
      if (!decision.opaque_lifecycle_policy_present) {
        return {NativeDomainDatatypeDecisionStatus::opaque_lifecycle_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::locator_domain:
      if (!decision.locator_authority_policy_present) {
        return {NativeDomainDatatypeDecisionStatus::locator_policy_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::cxx_udr_bridge:
      if (ExecutionDataPacketUuidIsNil(decision.cxx_udr_package_uuid)) {
        return {NativeDomainDatatypeDecisionStatus::cxx_udr_package_missing,
                decision_index};
      }
      if (!decision.non_cpp_udr_forbidden) {
        return {NativeDomainDatatypeDecisionStatus::non_cpp_udr_allowed,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::render_only_metadata:
      if (!decision.render_only_execution_forbidden) {
        return {NativeDomainDatatypeDecisionStatus::render_only_policy_missing,
                decision_index};
      }
      break;
    case NativeDomainDecisionClass::version_deferred:
    case NativeDomainDecisionClass::policy_unsupported:
      if (decision.unsupported_reason.empty() ||
          ExecutionDataPacketUuidIsNil(decision.diagnostic_profile_uuid)) {
        return {NativeDomainDatatypeDecisionStatus::unsupported_reason_missing,
                decision_index};
      }
      break;
  }
  if (!decision.parser_name_not_identity) {
    return {NativeDomainDatatypeDecisionStatus::authority_invariant_violation,
            decision_index};
  }
  if (NativeDomainConformanceStatusIsImplementationReady(
          decision.conformance_status) &&
      !decision.implementation_trace_anchor_present) {
    return {NativeDomainDatatypeDecisionStatus::decision_identity_incomplete,
            decision_index};
  }
  if (!NativeDomainHasMappingForDecision(registry, decision.decision_uuid)) {
    return {NativeDomainDatatypeDecisionStatus::reference_mapping_missing,
            decision_index};
  }
  for (std::size_t other_index = decision_index + 1;
       other_index < registry.decisions.size(); ++other_index) {
    const auto& other = registry.decisions[other_index];
    if (decision.scratchbird_family_name == other.scratchbird_family_name ||
        ExecutionDataPacketUuidEquals(decision.decision_uuid,
                                      other.decision_uuid)) {
      return {NativeDomainDatatypeDecisionStatus::decision_duplicate,
              other_index};
    }
  }
  return {};
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainReferenceMapping(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const ReferenceDatatypeMappingRecord& mapping,
    std::size_t mapping_index) {
  const auto* decision =
      NativeDomainFindDecisionByUuid(registry,
                                     mapping.scratchbird_decision_uuid);
  if (ExecutionDataPacketUuidIsNil(mapping.mapping_uuid) ||
      !NativeDomainReferenceFamilyIsValid(mapping.reference_family) ||
      mapping.reference_version_range.empty() ||
      mapping.reference_type_name.empty() ||
      !NativeDomainReferenceTypeCategoryIsValid(mapping.reference_type_category) ||
      mapping.mapping_hash.empty()) {
    return {NativeDomainDatatypeDecisionStatus::reference_mapping_incomplete, 0,
            mapping_index};
  }
  if (decision == nullptr) {
    return {NativeDomainDatatypeDecisionStatus::reference_mapping_orphan, 0,
            mapping_index};
  }
  if (!NativeDomainUnsupportedBehaviorIsValid(mapping.unsupported_behavior)) {
    return {NativeDomainDatatypeDecisionStatus::unsupported_behavior_missing,
            0, mapping_index};
  }
  if (NativeDomainDecisionClassIsUnsupportedOrDeferred(
          decision->decision_class)) {
    if (mapping.unsupported_behavior ==
            NativeDomainUnsupportedBehavior::none ||
        ExecutionDataPacketUuidIsNil(mapping.diagnostic_policy_uuid) ||
        mapping.conformance_test_id.empty()) {
      return {NativeDomainDatatypeDecisionStatus::unsupported_behavior_missing,
              0, mapping_index};
    }
    return {};
  }
  if (mapping.parser_accepts_type &&
      (ExecutionDataPacketUuidIsNil(mapping.literal_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.bind_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.cast_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.operation_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.index_statistics_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.storage_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.transport_policy_uuid) ||
       ExecutionDataPacketUuidIsNil(mapping.diagnostic_policy_uuid))) {
    return {NativeDomainDatatypeDecisionStatus::reference_mapping_policy_missing,
            0, mapping_index};
  }
  if (mapping.full_support_claimed && mapping.conformance_test_id.empty()) {
    return {NativeDomainDatatypeDecisionStatus::full_support_overclaim, 0,
            mapping_index};
  }
  return {};
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainCompoundElement(
    const NativeDomainDatatypeDecisionRegistry& registry,
    const CompoundDomainElementRecord& element,
    std::size_t mapping_index) {
  const auto* decision =
      NativeDomainFindDecisionByUuid(registry, element.decision_uuid);
  if (ExecutionDataPacketUuidIsNil(element.element_uuid) ||
      decision == nullptr ||
      decision->decision_class != NativeDomainDecisionClass::compound_domain ||
      element.element_name.empty() || element.path_expression.empty() ||
      element.nullability_policy.empty() ||
      ExecutionDataPacketUuidIsNil(element.cast_rule_uuid) ||
      ExecutionDataPacketUuidIsNil(element.operation_rule_uuid) ||
      element.versioning_rule.empty() || element.element_hash.empty() ||
      !element.parser_name_not_identity) {
    return {NativeDomainDatatypeDecisionStatus::compound_element_invalid, 0,
            mapping_index};
  }
  return {};
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainFullSupportClaim(
    const ReferenceFamilyFullSupportClaimRecord& claim,
    std::size_t mapping_index) {
  if (ExecutionDataPacketUuidIsNil(claim.claim_uuid) ||
      !NativeDomainReferenceFamilyIsValid(claim.reference_family) ||
      claim.expected_type_row_count == 0 ||
      claim.conformance_manifest_hash.empty() || claim.claim_hash.empty()) {
    return {NativeDomainDatatypeDecisionStatus::full_support_overclaim, 0,
            mapping_index};
  }
  if (claim.full_support_claimed &&
      claim.mapped_type_row_count < claim.expected_type_row_count) {
    return {NativeDomainDatatypeDecisionStatus::full_support_overclaim, 0,
            mapping_index};
  }
  return {};
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainConformanceGate(
    const NativeDomainConformanceGateRecord& gate,
    std::size_t gate_index) {
  if (ExecutionDataPacketUuidIsNil(gate.gate_uuid) || gate.gate_id.empty() ||
      !NativeDomainGateStatusIsValid(gate.status) ||
      gate.evidence_hash.empty() || gate.ctest_name.empty() ||
      gate.gate_hash.empty()) {
    return {NativeDomainDatatypeDecisionStatus::conformance_gate_missing, 0,
            0, gate_index};
  }
  if (gate.status != NativeDomainGateStatus::passed) {
    return {NativeDomainDatatypeDecisionStatus::conformance_gate_failed, 0,
            0, gate_index};
  }
  return {};
}

inline NativeDomainDatatypeDecisionValidationResult
ValidateNativeDomainDatatypeDecisionRegistry(
    const NativeDomainDatatypeDecisionRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {NativeDomainDatatypeDecisionStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {NativeDomainDatatypeDecisionStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {NativeDomainDatatypeDecisionStatus::registry_name_required};
  }
  if (registry.root_search_key !=
      "NVD-NATIVE-VS-DOMAIN-DATATYPE-DECISIONS") {
    return {NativeDomainDatatypeDecisionStatus::root_search_key_required};
  }
  if (registry.decisions.empty()) {
    return {NativeDomainDatatypeDecisionStatus::decision_records_required};
  }
  if (registry.reference_mappings.empty()) {
    return {NativeDomainDatatypeDecisionStatus::reference_mapping_records_required};
  }
  if (registry.conformance_gates.empty()) {
    return {NativeDomainDatatypeDecisionStatus::
                conformance_gate_records_required};
  }
  if (!registry.descriptor_authority_preserved ||
      !registry.uuid_identity_preserved ||
      !registry.parser_sblr_boundary_preserved ||
      !registry.reference_compatibility_not_authority ||
      !registry.non_cpp_udr_forbidden ||
      !registry.unsupported_diagnostics_required ||
      !registry.full_support_requires_complete_mappings) {
    return {NativeDomainDatatypeDecisionStatus::authority_invariant_violation};
  }
  constexpr std::string_view required_native_families[] = {
      "boolean",
      "signed_integer",
      "unsigned_integer",
      "exact_numeric_integer_foundation",
      "decimal_floating",
      "approximate_numeric",
      "text",
      "binary_string",
      "bit_string",
      "temporal",
      "interval_duration",
      "uuid_guid",
      "network_address",
      "lob_oversized_value",
      "array_list",
      "map_dictionary",
      "row_composite_struct",
      "variant_tagged_union",
      "range_multirange",
      "document",
      "full_text_search",
      "spatial",
      "vector_similarity",
      "graph",
      "time_series",
      "columnar_olap_wrapper",
      "aggregate_state",
      "approximate_sketch",
      "locator_envelope",
      "opaque_extension_envelope"};
  for (std::string_view family_name : required_native_families) {
    if (!NativeDomainHasDecisionFamily(registry, family_name)) {
      return {NativeDomainDatatypeDecisionStatus::decision_record_missing};
    }
  }
  constexpr NativeDomainDecisionClass required_classes[] = {
      NativeDomainDecisionClass::native_substrate,
      NativeDomainDecisionClass::domain_over_native,
      NativeDomainDecisionClass::compound_domain,
      NativeDomainDecisionClass::opaque_domain,
      NativeDomainDecisionClass::locator_domain,
      NativeDomainDecisionClass::cxx_udr_bridge,
      NativeDomainDecisionClass::render_only_metadata,
      NativeDomainDecisionClass::version_deferred,
      NativeDomainDecisionClass::policy_unsupported};
  for (const auto decision_class : required_classes) {
    if (!NativeDomainHasDecisionClass(registry, decision_class)) {
      return {NativeDomainDatatypeDecisionStatus::decision_record_missing};
    }
  }
  for (std::size_t mapping_index = 0;
       mapping_index < registry.compound_elements.size(); ++mapping_index) {
    const auto element_result = ValidateNativeDomainCompoundElement(
        registry, registry.compound_elements[mapping_index], mapping_index);
    if (!element_result.ok()) {
      return element_result;
    }
  }
  for (std::size_t decision_index = 0;
       decision_index < registry.decisions.size(); ++decision_index) {
    const auto decision_result = ValidateNativeDomainDecision(
        registry, registry.decisions[decision_index], decision_index);
    if (!decision_result.ok()) {
      return decision_result;
    }
  }
  for (std::size_t mapping_index = 0;
       mapping_index < registry.reference_mappings.size(); ++mapping_index) {
    const auto mapping_result = ValidateNativeDomainReferenceMapping(
        registry, registry.reference_mappings[mapping_index], mapping_index);
    if (!mapping_result.ok()) {
      return mapping_result;
    }
  }
  for (std::size_t mapping_index = 0;
       mapping_index < registry.full_support_claims.size(); ++mapping_index) {
    const auto claim_result = ValidateNativeDomainFullSupportClaim(
        registry.full_support_claims[mapping_index], mapping_index);
    if (!claim_result.ok()) {
      return claim_result;
    }
  }
  for (std::size_t gate_index = 0;
       gate_index < registry.conformance_gates.size(); ++gate_index) {
    const auto gate_result = ValidateNativeDomainConformanceGate(
        registry.conformance_gates[gate_index], gate_index);
    if (!gate_result.ok()) {
      return gate_result;
    }
  }
  constexpr std::string_view required_gates[] = {
      "NVD-GATE-001", "NVD-GATE-002", "NVD-GATE-003", "NVD-GATE-004",
      "NVD-CONF-001", "NVD-CONF-002", "NVD-CONF-003", "NVD-CONF-004",
      "NVD-CONF-005", "NVD-CONF-006", "NVD-CONF-007", "NVD-CONF-008",
      "NVD-CONF-009", "NVD-CONF-010", "NVD-CONF-011", "NVD-CONF-012",
      "NVD-CONF-013", "NVD-CONF-014", "NVD-CONF-015"};
  for (std::string_view gate_id : required_gates) {
    if (!NativeDomainHasPassedGate(registry, gate_id)) {
      return {NativeDomainDatatypeDecisionStatus::conformance_gate_missing};
    }
  }
  constexpr std::string_view required_diagnostics[] = {
      "SB_DIAG_DATATYPE_DECISION_MISSING",
      "SB_DIAG_DATATYPE_DECISION_INVALID",
      "SB_DIAG_DATATYPE_NATIVE_REQUIRED",
      "SB_DIAG_DATATYPE_DOMAIN_FORBIDDEN",
      "SB_DIAG_DATATYPE_DESCRIPTOR_MISSING",
      "SB_DIAG_DATATYPE_UNSUPPORTED_BY_VERSION",
      "SB_DIAG_DATATYPE_UNSUPPORTED_BY_POLICY",
      "SB_DIAG_DATATYPE_UNSUPPORTED_BY_PROFILE",
      "SB_DIAG_DATATYPE_CXX_UDR_REQUIRED",
      "SB_DIAG_DATATYPE_NON_CPP_UDR_FORBIDDEN",
      "SB_DIAG_DATATYPE_COMPOUND_ADDRESS_INVALID",
      "SB_DIAG_DATATYPE_FULL_SUPPORT_OVERCLAIM"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!NativeDomainHasDiagnosticCode(registry, diagnostic_code)) {
      return {NativeDomainDatatypeDecisionStatus::diagnostic_vector_missing};
    }
  }
  constexpr std::string_view required_metrics[] = {
      "sys.metrics.datatype.decisions.total",
      "sys.metrics.datatype.decisions.native_total",
      "sys.metrics.datatype.decisions.domain_total",
      "sys.metrics.datatype.decisions.deferred_total",
      "sys.metrics.datatype.decisions.unsupported_total",
      "sys.metrics.datatype.reference_mappings.total",
      "sys.metrics.datatype.reference_mappings.incomplete_total",
      "sys.metrics.datatype.diagnostics.unsupported_by_version_total",
      "sys.metrics.datatype.diagnostics.unsupported_by_policy_total",
      "sys.metrics.datatype.diagnostics.requires_cxx_udr_total",
      "sys.metrics.datatype.conformance.full_support_overclaim_total"};
  for (std::string_view metric : required_metrics) {
    if (!NativeDomainHasMetric(registry, metric)) {
      return {NativeDomainDatatypeDecisionStatus::local_metric_missing};
    }
  }
  if (!registry.cluster_metrics_guarded_by_cluster_governance) {
    return {NativeDomainDatatypeDecisionStatus::cluster_metric_guard_required};
  }
  return {};
}

// SEARCH_KEY: CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION
// SEARCH_KEY: CTB-CANONICAL-TEXT-BINARY-BIT-DATATYPES
// SEARCH_KEY: CTI-CANONICAL-TEMPORAL-INTERVAL-DATATYPES
// SEARCH_KEY: CINL-CANONICAL-IDENTITY-NETWORK-LOB-DATATYPES
// SEARCH_KEY: CSCR-CANONICAL-STRUCTURED-CONTAINER-RANGE-DATATYPES
// SEARCH_KEY: CDSSV-CANONICAL-DOCUMENT-SEARCH-SPATIAL-VECTOR-DATATYPES
// SEARCH_KEY: CGTC-CANONICAL-GRAPH-TIMESERIES-COLUMNAR-DATATYPES
// SEARCH_KEY: CASLO-CANONICAL-AGGREGATE-SKETCH-LOCATOR-OPAQUE-DATATYPES
enum class CanonicalDatatypeFamilyGroup : std::uint8_t {
  text_binary_bit = 0,
  temporal_interval = 1,
  identity_network_lob = 2,
  structured_container_range = 3,
  document_search_spatial_vector = 4,
  graph_timeseries_columnar = 5,
  aggregate_sketch_locator_opaque = 6
};

constexpr bool CanonicalDatatypeFamilyGroupIsValid(
    CanonicalDatatypeFamilyGroup group) noexcept {
  switch (group) {
    case CanonicalDatatypeFamilyGroup::text_binary_bit:
    case CanonicalDatatypeFamilyGroup::temporal_interval:
    case CanonicalDatatypeFamilyGroup::identity_network_lob:
    case CanonicalDatatypeFamilyGroup::structured_container_range:
    case CanonicalDatatypeFamilyGroup::document_search_spatial_vector:
    case CanonicalDatatypeFamilyGroup::graph_timeseries_columnar:
    case CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque:
      return true;
  }
  return false;
}

constexpr std::string_view CanonicalDatatypeFamilyGroupSearchKey(
    CanonicalDatatypeFamilyGroup group) noexcept {
  switch (group) {
    case CanonicalDatatypeFamilyGroup::text_binary_bit:
      return "CTB-CANONICAL-TEXT-BINARY-BIT-DATATYPES";
    case CanonicalDatatypeFamilyGroup::temporal_interval:
      return "CTI-CANONICAL-TEMPORAL-INTERVAL-DATATYPES";
    case CanonicalDatatypeFamilyGroup::identity_network_lob:
      return "CINL-CANONICAL-IDENTITY-NETWORK-LOB-DATATYPES";
    case CanonicalDatatypeFamilyGroup::structured_container_range:
      return "CSCR-CANONICAL-STRUCTURED-CONTAINER-RANGE-DATATYPES";
    case CanonicalDatatypeFamilyGroup::document_search_spatial_vector:
      return "CDSSV-CANONICAL-DOCUMENT-SEARCH-SPATIAL-VECTOR-DATATYPES";
    case CanonicalDatatypeFamilyGroup::graph_timeseries_columnar:
      return "CGTC-CANONICAL-GRAPH-TIMESERIES-COLUMNAR-DATATYPES";
    case CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque:
      return "CASLO-CANONICAL-AGGREGATE-SKETCH-LOCATOR-OPAQUE-DATATYPES";
  }
  return "";
}

enum class CanonicalDatatypeFamilyGateStatus : std::uint8_t {
  passed = 0,
  failed = 1,
  blocked = 2,
  deferred = 3
};

constexpr bool CanonicalDatatypeFamilyGateStatusIsValid(
    CanonicalDatatypeFamilyGateStatus status) noexcept {
  switch (status) {
    case CanonicalDatatypeFamilyGateStatus::passed:
    case CanonicalDatatypeFamilyGateStatus::failed:
    case CanonicalDatatypeFamilyGateStatus::blocked:
    case CanonicalDatatypeFamilyGateStatus::deferred:
      return true;
  }
  return false;
}

struct CanonicalDatatypeFamilyDescriptorRecord {
  Uuid descriptor_uuid;
  CanonicalDatatypeFamilyGroup family_group =
      CanonicalDatatypeFamilyGroup::text_binary_bit;
  std::string family_name;
  std::string descriptor_record_name;
  std::string controlling_search_key;
  std::uint64_t descriptor_version = 1;
  bool canonical_family_declared = true;
  bool descriptor_fields_complete = true;
  bool resource_epoch_bound = true;
  bool literal_bind_policy_present = true;
  bool cast_policy_present = true;
  bool operation_profile_present = true;
  bool storage_profile_present = true;
  bool index_profile_present = true;
  bool statistics_profile_present = true;
  bool metadata_profile_present = true;
  bool backup_restore_profile_present = true;
  bool replication_transport_profile_present = true;
  bool cluster_transport_profile_present = true;
  bool diagnostic_profile_present = true;
  bool metric_profile_present = true;
  bool conformance_profile_present = true;
  bool reference_mapping_required_or_refused = true;
  bool merge_policy_present = true;
  bool parser_render_not_authority = true;
  bool write_after_delta_not_recovery_authority = true;
  bool mga_visibility_preserved = true;
  bool uuid_identity_preserved = true;
  bool security_policy_preserved = true;
  bool fail_closed_when_dependency_missing = true;
  bool implementation_trace_anchor_present = true;
  std::string descriptor_hash;
};

struct CanonicalDatatypeFamilyOperationContractRecord {
  Uuid operation_contract_uuid;
  CanonicalDatatypeFamilyGroup family_group =
      CanonicalDatatypeFamilyGroup::text_binary_bit;
  std::string operation_contract_key;
  bool equality_hash_declared = true;
  bool ordering_policy_declared = true;
  bool cast_policy_declared = true;
  bool mutation_or_update_policy_declared = true;
  bool index_statistics_admission_declared = true;
  bool backup_transport_declared = true;
  bool merge_reconciliation_declared = true;
  bool reference_compatibility_declared = true;
  bool unsupported_behavior_diagnostic = true;
  bool descriptor_bound = true;
  bool parser_text_not_authority = true;
  bool non_cpp_udr_forbidden_when_trusted = true;
  std::string operation_contract_hash;
};

struct CanonicalDatatypeFamilyTransportRecord {
  Uuid transport_profile_uuid;
  CanonicalDatatypeFamilyGroup family_group =
      CanonicalDatatypeFamilyGroup::text_binary_bit;
  std::string transport_profile_key;
  bool backup_restore_declared = true;
  bool archive_declared = true;
  bool replication_declared = true;
  bool cluster_transport_declared = true;
  bool optional_delta_declared = true;
  bool descriptor_identity_preserved = true;
  bool resource_epoch_preserved = true;
  bool deterministic_refusal_declared = true;
  bool write_after_delta_not_recovery_authority = true;
  std::string transport_profile_hash;
};

struct CanonicalDatatypeFamilyConformanceGateRecord {
  Uuid gate_uuid;
  CanonicalDatatypeFamilyGroup family_group =
      CanonicalDatatypeFamilyGroup::text_binary_bit;
  std::string gate_id;
  CanonicalDatatypeFamilyGateStatus status =
      CanonicalDatatypeFamilyGateStatus::passed;
  std::string evidence_hash;
  std::string ctest_name;
  std::string gate_hash;
};

struct CanonicalDatatypeFamilyRegistry {
  Uuid registry_uuid;
  std::uint64_t registry_epoch = 1;
  std::string registry_name;
  std::string root_search_key =
      "CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION";
  std::vector<CanonicalDatatypeFamilyDescriptorRecord> descriptors;
  std::vector<CanonicalDatatypeFamilyOperationContractRecord>
      operation_contracts;
  std::vector<CanonicalDatatypeFamilyTransportRecord> transport_profiles;
  std::vector<CanonicalDatatypeFamilyConformanceGateRecord>
      conformance_gates;
  std::vector<std::string> diagnostic_codes;
  std::vector<std::string> local_metric_names;
  bool descriptor_authority_preserved = true;
  bool uuid_identity_preserved = true;
  bool parser_sblr_boundary_preserved = true;
  bool reference_compatibility_not_authority = true;
  bool non_cpp_udr_forbidden = true;
  bool unsupported_diagnostics_required = true;
  bool cluster_metrics_guarded_by_cluster_governance = true;
  bool write_after_delta_not_recovery_authority = true;
  bool mga_visibility_preserved = true;
};

enum class CanonicalDatatypeFamilyStatus : std::uint16_t {
  ok = 0,
  registry_uuid_required = 1,
  registry_epoch_required = 2,
  registry_name_required = 3,
  root_search_key_required = 4,
  descriptor_records_required = 5,
  operation_contract_records_required = 6,
  transport_profile_records_required = 7,
  conformance_gate_records_required = 8,
  authority_invariant_violation = 9,
  descriptor_record_missing = 10,
  descriptor_uuid_required = 11,
  descriptor_identity_incomplete = 12,
  descriptor_group_invalid = 13,
  descriptor_search_key_mismatch = 14,
  descriptor_contract_incomplete = 15,
  descriptor_duplicate = 16,
  operation_contract_missing = 17,
  operation_contract_incomplete = 18,
  transport_profile_missing = 19,
  transport_profile_incomplete = 20,
  transport_recovery_authority_violation = 21,
  conformance_gate_missing = 22,
  conformance_gate_failed = 23,
  diagnostic_vector_missing = 24,
  local_metric_missing = 25,
  cluster_metric_guard_required = 26
};

constexpr std::string_view CanonicalDatatypeFamilyStatusName(
    CanonicalDatatypeFamilyStatus status) noexcept {
  switch (status) {
    case CanonicalDatatypeFamilyStatus::ok:
      return "ok";
    case CanonicalDatatypeFamilyStatus::registry_uuid_required:
      return "registry_uuid_required";
    case CanonicalDatatypeFamilyStatus::registry_epoch_required:
      return "registry_epoch_required";
    case CanonicalDatatypeFamilyStatus::registry_name_required:
      return "registry_name_required";
    case CanonicalDatatypeFamilyStatus::root_search_key_required:
      return "root_search_key_required";
    case CanonicalDatatypeFamilyStatus::descriptor_records_required:
      return "descriptor_records_required";
    case CanonicalDatatypeFamilyStatus::operation_contract_records_required:
      return "operation_contract_records_required";
    case CanonicalDatatypeFamilyStatus::transport_profile_records_required:
      return "transport_profile_records_required";
    case CanonicalDatatypeFamilyStatus::conformance_gate_records_required:
      return "conformance_gate_records_required";
    case CanonicalDatatypeFamilyStatus::authority_invariant_violation:
      return "authority_invariant_violation";
    case CanonicalDatatypeFamilyStatus::descriptor_record_missing:
      return "descriptor_record_missing";
    case CanonicalDatatypeFamilyStatus::descriptor_uuid_required:
      return "descriptor_uuid_required";
    case CanonicalDatatypeFamilyStatus::descriptor_identity_incomplete:
      return "descriptor_identity_incomplete";
    case CanonicalDatatypeFamilyStatus::descriptor_group_invalid:
      return "descriptor_group_invalid";
    case CanonicalDatatypeFamilyStatus::descriptor_search_key_mismatch:
      return "descriptor_search_key_mismatch";
    case CanonicalDatatypeFamilyStatus::descriptor_contract_incomplete:
      return "descriptor_contract_incomplete";
    case CanonicalDatatypeFamilyStatus::descriptor_duplicate:
      return "descriptor_duplicate";
    case CanonicalDatatypeFamilyStatus::operation_contract_missing:
      return "operation_contract_missing";
    case CanonicalDatatypeFamilyStatus::operation_contract_incomplete:
      return "operation_contract_incomplete";
    case CanonicalDatatypeFamilyStatus::transport_profile_missing:
      return "transport_profile_missing";
    case CanonicalDatatypeFamilyStatus::transport_profile_incomplete:
      return "transport_profile_incomplete";
    case CanonicalDatatypeFamilyStatus::transport_recovery_authority_violation:
      return "transport_recovery_authority_violation";
    case CanonicalDatatypeFamilyStatus::conformance_gate_missing:
      return "conformance_gate_missing";
    case CanonicalDatatypeFamilyStatus::conformance_gate_failed:
      return "conformance_gate_failed";
    case CanonicalDatatypeFamilyStatus::diagnostic_vector_missing:
      return "diagnostic_vector_missing";
    case CanonicalDatatypeFamilyStatus::local_metric_missing:
      return "local_metric_missing";
    case CanonicalDatatypeFamilyStatus::cluster_metric_guard_required:
      return "cluster_metric_guard_required";
  }
  return "unknown";
}

struct CanonicalDatatypeFamilyValidationResult {
  CanonicalDatatypeFamilyStatus status = CanonicalDatatypeFamilyStatus::ok;
  std::size_t descriptor_index = 0;
  std::size_t operation_index = 0;
  std::size_t transport_index = 0;
  std::size_t gate_index = 0;

  bool ok() const noexcept {
    return status == CanonicalDatatypeFamilyStatus::ok;
  }
};

inline bool CanonicalDatatypeFamilyHasDescriptor(
    const CanonicalDatatypeFamilyRegistry& registry,
    CanonicalDatatypeFamilyGroup group,
    std::string_view family_name) noexcept {
  for (const auto& descriptor : registry.descriptors) {
    if (descriptor.family_group == group &&
        descriptor.family_name == family_name) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalDatatypeFamilyHasOperationContract(
    const CanonicalDatatypeFamilyRegistry& registry,
    CanonicalDatatypeFamilyGroup group) noexcept {
  for (const auto& contract : registry.operation_contracts) {
    if (contract.family_group == group) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalDatatypeFamilyHasTransportProfile(
    const CanonicalDatatypeFamilyRegistry& registry,
    CanonicalDatatypeFamilyGroup group) noexcept {
  for (const auto& profile : registry.transport_profiles) {
    if (profile.family_group == group) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalDatatypeFamilyHasPassedGate(
    const CanonicalDatatypeFamilyRegistry& registry,
    std::string_view gate_id) noexcept {
  for (const auto& gate : registry.conformance_gates) {
    if (gate.gate_id == gate_id) {
      return gate.status == CanonicalDatatypeFamilyGateStatus::passed &&
             !ExecutionDataPacketUuidIsNil(gate.gate_uuid) &&
             !gate.evidence_hash.empty() && !gate.ctest_name.empty() &&
             !gate.gate_hash.empty();
    }
  }
  return false;
}

inline bool CanonicalDatatypeFamilyHasDiagnosticCode(
    const CanonicalDatatypeFamilyRegistry& registry,
    std::string_view diagnostic_code) noexcept {
  for (const auto& code : registry.diagnostic_codes) {
    if (code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

inline bool CanonicalDatatypeFamilyHasMetric(
    const CanonicalDatatypeFamilyRegistry& registry,
    std::string_view metric_name) noexcept {
  for (const auto& metric : registry.local_metric_names) {
    if (metric == metric_name) {
      return true;
    }
  }
  return false;
}

inline CanonicalDatatypeFamilyValidationResult
ValidateCanonicalDatatypeFamilyDescriptor(
    const CanonicalDatatypeFamilyRegistry& registry,
    const CanonicalDatatypeFamilyDescriptorRecord& descriptor,
    std::size_t descriptor_index) {
  if (ExecutionDataPacketUuidIsNil(descriptor.descriptor_uuid)) {
    return {CanonicalDatatypeFamilyStatus::descriptor_uuid_required,
            descriptor_index};
  }
  if (descriptor.family_name.empty() ||
      descriptor.descriptor_record_name.empty() ||
      descriptor.controlling_search_key.empty() ||
      descriptor.descriptor_hash.empty() || descriptor.descriptor_version == 0) {
    return {CanonicalDatatypeFamilyStatus::descriptor_identity_incomplete,
            descriptor_index};
  }
  if (!CanonicalDatatypeFamilyGroupIsValid(descriptor.family_group)) {
    return {CanonicalDatatypeFamilyStatus::descriptor_group_invalid,
            descriptor_index};
  }
  if (descriptor.controlling_search_key !=
      CanonicalDatatypeFamilyGroupSearchKey(descriptor.family_group)) {
    return {CanonicalDatatypeFamilyStatus::descriptor_search_key_mismatch,
            descriptor_index};
  }
  if (!descriptor.canonical_family_declared ||
      !descriptor.descriptor_fields_complete ||
      !descriptor.resource_epoch_bound ||
      !descriptor.literal_bind_policy_present ||
      !descriptor.cast_policy_present ||
      !descriptor.operation_profile_present ||
      !descriptor.storage_profile_present ||
      !descriptor.index_profile_present ||
      !descriptor.statistics_profile_present ||
      !descriptor.metadata_profile_present ||
      !descriptor.backup_restore_profile_present ||
      !descriptor.replication_transport_profile_present ||
      !descriptor.cluster_transport_profile_present ||
      !descriptor.diagnostic_profile_present ||
      !descriptor.metric_profile_present ||
      !descriptor.conformance_profile_present ||
      !descriptor.reference_mapping_required_or_refused ||
      !descriptor.merge_policy_present ||
      !descriptor.parser_render_not_authority ||
      !descriptor.write_after_delta_not_recovery_authority ||
      !descriptor.mga_visibility_preserved ||
      !descriptor.uuid_identity_preserved ||
      !descriptor.security_policy_preserved ||
      !descriptor.fail_closed_when_dependency_missing ||
      !descriptor.implementation_trace_anchor_present) {
    return {CanonicalDatatypeFamilyStatus::descriptor_contract_incomplete,
            descriptor_index};
  }
  if (!CanonicalDatatypeFamilyHasOperationContract(registry,
                                                  descriptor.family_group)) {
    return {CanonicalDatatypeFamilyStatus::operation_contract_missing,
            descriptor_index};
  }
  if (!CanonicalDatatypeFamilyHasTransportProfile(registry,
                                                 descriptor.family_group)) {
    return {CanonicalDatatypeFamilyStatus::transport_profile_missing,
            descriptor_index};
  }
  for (std::size_t other_index = descriptor_index + 1;
       other_index < registry.descriptors.size(); ++other_index) {
    const auto& other = registry.descriptors[other_index];
    if ((descriptor.family_group == other.family_group &&
         descriptor.family_name == other.family_name) ||
        ExecutionDataPacketUuidEquals(descriptor.descriptor_uuid,
                                      other.descriptor_uuid)) {
      return {CanonicalDatatypeFamilyStatus::descriptor_duplicate,
              other_index};
    }
  }
  return {};
}

inline CanonicalDatatypeFamilyValidationResult
ValidateCanonicalDatatypeFamilyOperationContract(
    const CanonicalDatatypeFamilyOperationContractRecord& contract,
    std::size_t operation_index) {
  if (ExecutionDataPacketUuidIsNil(contract.operation_contract_uuid) ||
      !CanonicalDatatypeFamilyGroupIsValid(contract.family_group) ||
      contract.operation_contract_key.empty() ||
      contract.operation_contract_hash.empty() ||
      !contract.equality_hash_declared ||
      !contract.ordering_policy_declared ||
      !contract.cast_policy_declared ||
      !contract.mutation_or_update_policy_declared ||
      !contract.index_statistics_admission_declared ||
      !contract.backup_transport_declared ||
      !contract.merge_reconciliation_declared ||
      !contract.reference_compatibility_declared ||
      !contract.unsupported_behavior_diagnostic ||
      !contract.descriptor_bound ||
      !contract.parser_text_not_authority ||
      !contract.non_cpp_udr_forbidden_when_trusted) {
    return {CanonicalDatatypeFamilyStatus::operation_contract_incomplete, 0,
            operation_index};
  }
  return {};
}

inline CanonicalDatatypeFamilyValidationResult
ValidateCanonicalDatatypeFamilyTransportProfile(
    const CanonicalDatatypeFamilyTransportRecord& profile,
    std::size_t transport_index) {
  if (ExecutionDataPacketUuidIsNil(profile.transport_profile_uuid) ||
      !CanonicalDatatypeFamilyGroupIsValid(profile.family_group) ||
      profile.transport_profile_key.empty() ||
      profile.transport_profile_hash.empty() ||
      !profile.backup_restore_declared || !profile.archive_declared ||
      !profile.replication_declared ||
      !profile.cluster_transport_declared ||
      !profile.optional_delta_declared ||
      !profile.descriptor_identity_preserved ||
      !profile.resource_epoch_preserved ||
      !profile.deterministic_refusal_declared) {
    return {CanonicalDatatypeFamilyStatus::transport_profile_incomplete, 0,
            0, transport_index};
  }
  if (!profile.write_after_delta_not_recovery_authority) {
    return {CanonicalDatatypeFamilyStatus::
                transport_recovery_authority_violation,
            0, 0, transport_index};
  }
  return {};
}

inline CanonicalDatatypeFamilyValidationResult
ValidateCanonicalDatatypeFamilyGate(
    const CanonicalDatatypeFamilyConformanceGateRecord& gate,
    std::size_t gate_index) {
  if (ExecutionDataPacketUuidIsNil(gate.gate_uuid) ||
      !CanonicalDatatypeFamilyGroupIsValid(gate.family_group) ||
      gate.gate_id.empty() ||
      !CanonicalDatatypeFamilyGateStatusIsValid(gate.status) ||
      gate.evidence_hash.empty() || gate.ctest_name.empty() ||
      gate.gate_hash.empty()) {
    return {CanonicalDatatypeFamilyStatus::conformance_gate_missing, 0, 0, 0,
            gate_index};
  }
  if (gate.status != CanonicalDatatypeFamilyGateStatus::passed) {
    return {CanonicalDatatypeFamilyStatus::conformance_gate_failed, 0, 0, 0,
            gate_index};
  }
  return {};
}

inline CanonicalDatatypeFamilyValidationResult
ValidateCanonicalDatatypeFamilyRegistry(
    const CanonicalDatatypeFamilyRegistry& registry) {
  if (ExecutionDataPacketUuidIsNil(registry.registry_uuid)) {
    return {CanonicalDatatypeFamilyStatus::registry_uuid_required};
  }
  if (registry.registry_epoch == 0) {
    return {CanonicalDatatypeFamilyStatus::registry_epoch_required};
  }
  if (registry.registry_name.empty()) {
    return {CanonicalDatatypeFamilyStatus::registry_name_required};
  }
  if (registry.root_search_key !=
      "CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION") {
    return {CanonicalDatatypeFamilyStatus::root_search_key_required};
  }
  if (registry.descriptors.empty()) {
    return {CanonicalDatatypeFamilyStatus::descriptor_records_required};
  }
  if (registry.operation_contracts.empty()) {
    return {
        CanonicalDatatypeFamilyStatus::operation_contract_records_required};
  }
  if (registry.transport_profiles.empty()) {
    return {CanonicalDatatypeFamilyStatus::transport_profile_records_required};
  }
  if (registry.conformance_gates.empty()) {
    return {CanonicalDatatypeFamilyStatus::conformance_gate_records_required};
  }
  if (!registry.descriptor_authority_preserved ||
      !registry.uuid_identity_preserved ||
      !registry.parser_sblr_boundary_preserved ||
      !registry.reference_compatibility_not_authority ||
      !registry.non_cpp_udr_forbidden ||
      !registry.unsupported_diagnostics_required ||
      !registry.write_after_delta_not_recovery_authority ||
      !registry.mga_visibility_preserved) {
    return {CanonicalDatatypeFamilyStatus::authority_invariant_violation};
  }

  constexpr std::pair<CanonicalDatatypeFamilyGroup, std::string_view>
      required_descriptors[] = {
          {CanonicalDatatypeFamilyGroup::text_binary_bit, "text"},
          {CanonicalDatatypeFamilyGroup::text_binary_bit, "binary_string"},
          {CanonicalDatatypeFamilyGroup::text_binary_bit, "bit_string"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "date"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "time"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "time_with_zone"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "timestamp"},
          {CanonicalDatatypeFamilyGroup::temporal_interval,
           "timestamp_with_zone"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "instant"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "year_domain"},
          {CanonicalDatatypeFamilyGroup::temporal_interval,
           "year_month_interval"},
          {CanonicalDatatypeFamilyGroup::temporal_interval,
           "day_time_interval"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "mixed_interval"},
          {CanonicalDatatypeFamilyGroup::temporal_interval, "fixed_duration"},
          {CanonicalDatatypeFamilyGroup::temporal_interval,
           "reference_duration_domain"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "uuid"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "guid_domain"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "object_id_domain"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "timeuuid_domain"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "system_object_id"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "ip_address"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "ip_network"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "ip_range"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob, "mac_address"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "lob_oversized_value"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "external_value_handle"},
          {CanonicalDatatypeFamilyGroup::identity_network_lob,
           "stream_descriptor"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "enum"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "set"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "array"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "list"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "map"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "row"},
          {CanonicalDatatypeFamilyGroup::structured_container_range,
           "composite"},
          {CanonicalDatatypeFamilyGroup::structured_container_range,
           "variant"},
          {CanonicalDatatypeFamilyGroup::structured_container_range, "range"},
          {CanonicalDatatypeFamilyGroup::structured_container_range,
           "multirange"},
          {CanonicalDatatypeFamilyGroup::structured_container_range,
           "object_compatible_domain"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "document"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "token_stream"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "search_query"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "search_rank_feature"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "search_completion"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "search_percolator"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "geometry"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "geography"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "point"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "shape"},
          {CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
           "vector_similarity"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "graph_node"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "graph_edge"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "graph_path"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "graph_label"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "graph_property_map"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "measurement"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar, "tag_set"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "field_value"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "series_key"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "time_bucket"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "rollup_state"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "nullable_wrapper"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "dictionary_encoded"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "low_cardinality"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "nested_column"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "column_segment_value"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "compressed_column_value"},
          {CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
           "vectorized_batch_value"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "aggregate_state"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "hyperloglog_sketch"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "bloom_filter"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "quantile_sketch"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "histogram_sketch"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "ranking_summary"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "vector_search_summary"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "lob_locator"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "external_file_locator"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "remote_object_locator"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "bridge_handle"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "cursor_table_handle"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "system_pseudotype"},
          {CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque,
           "opaque_extension"}};
  for (const auto& required : required_descriptors) {
    if (!CanonicalDatatypeFamilyHasDescriptor(registry, required.first,
                                             required.second)) {
      return {CanonicalDatatypeFamilyStatus::descriptor_record_missing};
    }
  }

  constexpr CanonicalDatatypeFamilyGroup required_groups[] = {
      CanonicalDatatypeFamilyGroup::text_binary_bit,
      CanonicalDatatypeFamilyGroup::temporal_interval,
      CanonicalDatatypeFamilyGroup::identity_network_lob,
      CanonicalDatatypeFamilyGroup::structured_container_range,
      CanonicalDatatypeFamilyGroup::document_search_spatial_vector,
      CanonicalDatatypeFamilyGroup::graph_timeseries_columnar,
      CanonicalDatatypeFamilyGroup::aggregate_sketch_locator_opaque};
  for (const auto group : required_groups) {
    if (!CanonicalDatatypeFamilyHasOperationContract(registry, group)) {
      return {CanonicalDatatypeFamilyStatus::operation_contract_missing};
    }
    if (!CanonicalDatatypeFamilyHasTransportProfile(registry, group)) {
      return {CanonicalDatatypeFamilyStatus::transport_profile_missing};
    }
  }

  for (std::size_t operation_index = 0;
       operation_index < registry.operation_contracts.size();
       ++operation_index) {
    const auto operation_result =
        ValidateCanonicalDatatypeFamilyOperationContract(
            registry.operation_contracts[operation_index], operation_index);
    if (!operation_result.ok()) {
      return operation_result;
    }
  }
  for (std::size_t transport_index = 0;
       transport_index < registry.transport_profiles.size();
       ++transport_index) {
    const auto transport_result =
        ValidateCanonicalDatatypeFamilyTransportProfile(
            registry.transport_profiles[transport_index], transport_index);
    if (!transport_result.ok()) {
      return transport_result;
    }
  }
  for (std::size_t descriptor_index = 0;
       descriptor_index < registry.descriptors.size(); ++descriptor_index) {
    const auto descriptor_result = ValidateCanonicalDatatypeFamilyDescriptor(
        registry, registry.descriptors[descriptor_index], descriptor_index);
    if (!descriptor_result.ok()) {
      return descriptor_result;
    }
  }
  for (std::size_t gate_index = 0;
       gate_index < registry.conformance_gates.size(); ++gate_index) {
    const auto gate_result = ValidateCanonicalDatatypeFamilyGate(
        registry.conformance_gates[gate_index], gate_index);
    if (!gate_result.ok()) {
      return gate_result;
    }
  }

  constexpr std::string_view required_gates[] = {
      "CTB-GATE-001",   "CTB-GATE-002",   "CTB-GATE-003",
      "CTB-GATE-004",   "CTI-GATE-001",   "CTI-GATE-002",
      "CTI-GATE-003",   "CTI-GATE-004",   "CINL-GATE-001",
      "CINL-GATE-002",  "CINL-GATE-003",  "CINL-GATE-004",
      "CSCR-GATE-001",  "CSCR-GATE-002",  "CSCR-GATE-003",
      "CSCR-GATE-004",  "CDSSV-GATE-001", "CDSSV-GATE-002",
      "CDSSV-GATE-003", "CDSSV-GATE-004", "CGTC-GATE-001",
      "CGTC-GATE-002",  "CGTC-GATE-003",  "CGTC-GATE-004",
      "CASLO-GATE-001", "CASLO-GATE-002", "CASLO-GATE-003",
      "CASLO-GATE-004", "CTB-CONF-001",   "CTB-CONF-002",
      "CTB-CONF-003",   "CTB-CONF-004",   "CTB-CONF-005",
      "CTB-CONF-006",   "CTB-CONF-007",   "CTB-CONF-008",
      "CTB-CONF-009",   "CTI-CONF-001",   "CTI-CONF-002",
      "CTI-CONF-003",   "CTI-CONF-004",   "CTI-CONF-005",
      "CTI-CONF-006",   "CTI-CONF-007",   "CTI-CONF-008",
      "CTI-CONF-009",   "CTI-CONF-010",   "CTI-CONF-011",
      "CINL-CONF-001",  "CINL-CONF-002",  "CINL-CONF-003",
      "CINL-CONF-004",  "CINL-CONF-005",  "CINL-CONF-006",
      "CINL-CONF-007",  "CINL-CONF-008",  "CSCR-CONF-001",
      "CSCR-CONF-002",  "CSCR-CONF-003",  "CSCR-CONF-004",
      "CSCR-CONF-005",  "CSCR-CONF-006",  "CSCR-CONF-007",
      "CSCR-CONF-008",  "CSCR-CONF-009",  "CSCR-CONF-010",
      "CSCR-CONF-011",  "CSCR-CONF-012",  "CSCR-CONF-013",
      "CDSSV-CONF-001", "CDSSV-CONF-002", "CDSSV-CONF-003",
      "CDSSV-CONF-004", "CDSSV-CONF-005", "CDSSV-CONF-006",
      "CDSSV-CONF-007", "CDSSV-CONF-008", "CDSSV-CONF-009",
      "CDSSV-CONF-010", "CDSSV-CONF-011", "CGTC-CONF-001",
      "CGTC-CONF-002",  "CGTC-CONF-003",  "CGTC-CONF-004",
      "CGTC-CONF-005",  "CGTC-CONF-006",  "CGTC-CONF-007",
      "CGTC-CONF-008",  "CGTC-CONF-009",  "CGTC-CONF-010",
      "CASLO-CONF-001", "CASLO-CONF-002", "CASLO-CONF-003",
      "CASLO-CONF-004", "CASLO-CONF-005", "CASLO-CONF-006",
      "CASLO-CONF-007", "CASLO-CONF-008", "CASLO-CONF-009",
      "CASLO-CONF-010", "CASLO-CONF-011"};
  for (std::string_view gate_id : required_gates) {
    if (!CanonicalDatatypeFamilyHasPassedGate(registry, gate_id)) {
      return {CanonicalDatatypeFamilyStatus::conformance_gate_missing};
    }
  }

  constexpr std::string_view required_diagnostics[] = {
      "CTB.TEXT.DESCRIPTOR_INVALID",
      "CTB.TEXT.INVALID_ENCODING",
      "CTB.TEXT.COLLATION_RESOURCE_MISSING",
      "CTB.TEXT.LENGTH_EXCEEDED",
      "CTB.TEXT_BINARY.CAST_ENCODING_MISSING",
      "CTB.TEXT.RESOURCE_EPOCH_MISMATCH",
      "CTB.BINARY.DESCRIPTOR_INVALID",
      "CTB.BINARY.LENGTH_EXCEEDED",
      "CTB.BINARY.ORDERING_REFUSED",
      "CTB.BIT.DESCRIPTOR_INVALID",
      "CTB.BIT.LENGTH_EXCEEDED",
      "CTB.BIT.PADDING_REFUSED",
      "CTB.BIT.OPERATION_INCOMPATIBLE",
      "CTI.TEMPORAL.DESCRIPTOR_INVALID",
      "CTI.INTERVAL.DESCRIPTOR_INVALID",
      "CTI.TEMPORAL.INVALID_LITERAL",
      "CTI.TEMPORAL.PRECISION_LOSS",
      "CTI.TEMPORAL.TIMEZONE_RESOURCE_MISSING",
      "CTI.TEMPORAL.AMBIGUOUS_LOCAL_TIME",
      "CTI.TEMPORAL.NONEXISTENT_LOCAL_TIME",
      "CTI.TEMPORAL.ZERO_DATE_REFUSED",
      "CTI.TEMPORAL.LEAP_SECOND_REFUSED",
      "CTI.INTERVAL.SUBTYPE_MISMATCH",
      "CTI.INTERVAL.CALENDAR_OPERATION_REFUSED",
      "CINL.IDENTITY.DESCRIPTOR_INVALID",
      "CINL.IDENTITY.INVALID_LITERAL",
      "CINL.IDENTITY.REFERENCE_ORDERING_MISMATCH",
      "CINL.IDENTITY.GENERATION_FENCED",
      "CINL.NETWORK.DESCRIPTOR_INVALID",
      "CINL.NETWORK.INVALID_ADDRESS",
      "CINL.NETWORK.PREFIX_INVALID",
      "CINL.LOB.DESCRIPTOR_INVALID",
      "CINL.LOB.HANDLE_EXPIRED",
      "CINL.LOB.CHUNK_MISSING",
      "CINL.LOB.STREAM_BACKPRESSURE_EXHAUSTED",
      "CINL.LOCATOR.DEREFERENCE_REFUSED",
      "CINL.LOCATOR.EXPIRED",
      "CINL.LOCATOR.REVOKED",
      "CSCR.DESCRIPTOR.INVALID",
      "CSCR.ENUM.LABEL_UNKNOWN",
      "CSCR.SET.DUPLICATE_REFUSED",
      "CSCR.ARRAY.BOUNDS_INVALID",
      "CSCR.MAP.KEY_DUPLICATE_REFUSED",
      "CSCR.COMPOUND.FIELD_NOT_VISIBLE",
      "CSCR.DOMAIN_ELEMENT_PATH.STALE",
      "CSCR.VARIANT.TAG_UNKNOWN",
      "CSCR.RANGE.BOUND_INVALID",
      "CSCR.MULTIRANGE.NOT_CANONICAL",
      "CDSSV.DOCUMENT.DESCRIPTOR_INVALID",
      "CDSSV.DOCUMENT.INVALID_PAYLOAD",
      "CDSSV.DOCUMENT.PATH_MISSING",
      "CDSSV.DOCUMENT.DUPLICATE_KEY_REFUSED",
      "CDSSV.SEARCH.DESCRIPTOR_INVALID",
      "CDSSV.SEARCH.ANALYZER_MISSING",
      "CDSSV.SEARCH.QUERY_INVALID",
      "CDSSV.SPATIAL.DESCRIPTOR_INVALID",
      "CDSSV.SPATIAL.SRID_MISMATCH",
      "CDSSV.SPATIAL.INVALID_GEOMETRY",
      "CDSSV.VECTOR.DESCRIPTOR_INVALID",
      "CDSSV.VECTOR.DIMENSION_MISMATCH",
      "CDSSV.VECTOR.METRIC_MISMATCH",
      "CDSSV.VECTOR.QUANTIZATION_REFUSED",
      "CGTC.GRAPH.DESCRIPTOR_INVALID",
      "CGTC.GRAPH.LABEL_UNKNOWN",
      "CGTC.GRAPH.PATH_INVALID",
      "CGTC.GRAPH.PROPERTY_INVALID",
      "CGTC.TIMESERIES.DESCRIPTOR_INVALID",
      "CGTC.TIMESERIES.TIMESTAMP_PRECISION_MISMATCH",
      "CGTC.TIMESERIES.TAG_POLICY_VIOLATION",
      "CGTC.TIMESERIES.RETENTION_REFUSED",
      "CGTC.COLUMNAR.DESCRIPTOR_INVALID",
      "CGTC.COLUMNAR.DICTIONARY_MISSING",
      "CGTC.COLUMNAR.SEGMENT_CODEC_MISSING",
      "CGTC.COLUMNAR.WRAPPER_INCOMPATIBLE",
      "CASLO.AGGREGATE.DESCRIPTOR_INVALID",
      "CASLO.AGGREGATE.VERSION_MISMATCH",
      "CASLO.AGGREGATE.NOT_EXPOSED",
      "CASLO.SKETCH.DESCRIPTOR_INVALID",
      "CASLO.SKETCH.PARAMETER_MISMATCH",
      "CASLO.SKETCH.MERGE_REFUSED",
      "CASLO.LOCATOR.DESCRIPTOR_INVALID",
      "CASLO.LOCATOR.EXPIRED",
      "CASLO.LOCATOR.REVOKED",
      "CASLO.LOCATOR.DEREFERENCE_DENIED",
      "CASLO.PSEUDOTYPE.NOT_VISIBLE",
      "CASLO.OPAQUE.DESCRIPTOR_INVALID",
      "CASLO.OPAQUE.UDR_PACKAGE_MISSING",
      "CASLO.OPAQUE.CODEC_MISMATCH",
      "CASLO.OPAQUE.RAW_PAYLOAD_REFUSED",
      "CASLO.UDR.RUNTIME_FORBIDDEN"};
  for (std::string_view diagnostic_code : required_diagnostics) {
    if (!CanonicalDatatypeFamilyHasDiagnosticCode(registry,
                                                  diagnostic_code)) {
      return {CanonicalDatatypeFamilyStatus::diagnostic_vector_missing};
    }
  }

  constexpr std::string_view required_metrics[] = {
      "sys.metrics.datatypes.text.descriptor.admissions_total",
      "sys.metrics.datatypes.binary.length_refusals_total",
      "sys.metrics.datatypes.bit.operation.refusals_total",
      "sys.metrics.datatypes.temporal.invalid_literals_total",
      "sys.metrics.datatypes.interval.subtype_mismatches_total",
      "sys.metrics.datatypes.identity.generation_attempts_total",
      "sys.metrics.datatypes.network.invalid_addresses_total",
      "sys.metrics.datatypes.lob.handle_expirations_total",
      "sys.metrics.datatypes.external_locator.dereference_refusals_total",
      "sys.metrics.datatypes.structured.descriptor_admissions_total",
      "sys.metrics.datatypes.enum.unknown_labels_total",
      "sys.metrics.datatypes.container.invalid_bounds_total",
      "sys.metrics.datatypes.variant.unknown_tags_total",
      "sys.metrics.datatypes.range.invalid_bounds_total",
      "sys.metrics.datatypes.document.invalid_payloads_total",
      "sys.metrics.datatypes.search.analyzer_missing_total",
      "sys.metrics.datatypes.spatial.srid_mismatches_total",
      "sys.metrics.datatypes.vector.dimension_mismatches_total",
      "sys.metrics.datatypes.graph.label_misses_total",
      "sys.metrics.datatypes.timeseries.precision_mismatches_total",
      "sys.metrics.datatypes.columnar.dictionary_misses_total",
      "sys.metrics.datatypes.aggregate_state.version_mismatches_total",
      "sys.metrics.datatypes.sketch.parameter_mismatches_total",
      "sys.metrics.datatypes.locator.dereference_denials_total",
      "sys.metrics.datatypes.pseudotype.visibility_denials_total",
      "sys.metrics.datatypes.opaque.udr_package_misses_total",
      "sys.metrics.datatypes.family.transport_refusals_total",
      "sys.metrics.datatypes.family.reference_mapping_misses_total",
      "sys.metrics.datatypes.family.merge_manual_review_total"};
  for (std::string_view metric : required_metrics) {
    if (!CanonicalDatatypeFamilyHasMetric(registry, metric)) {
      return {CanonicalDatatypeFamilyStatus::local_metric_missing};
    }
  }
  if (!registry.cluster_metrics_guarded_by_cluster_governance) {
    return {CanonicalDatatypeFamilyStatus::cluster_metric_guard_required};
  }
  return {};
}

// SEARCH_KEY: EDR-CANONICAL-MODIFIER
// SEARCH_KEY: descriptor hash
// SEARCH_KEY: vector_layout
struct ExecutionTypeReferenceModifier {
  std::string name;
  std::string value;
};

struct ExecutionTypeModifierCanonicalizationInput {
  ExecutionTypeDescriptor descriptor;
  std::vector<ExecutionTypeReferenceModifier> reference_modifiers;
  bool range_subtype_present = false;
  bool array_element_present = false;
};

struct ExecutionTypeModifierCanonicalForm {
  ExecutionTypeDescriptor descriptor;
  std::vector<ExecutionTypeReferenceModifier> reference_modifiers;
  std::string canonical_modifier_key;
  std::string descriptor_identity_digest;
};

enum class ExecutionTypeModifierCanonicalizationStatus : std::uint8_t {
  ok = 0,
  descriptor_invalid = 1,
  precision_required = 2,
  precision_not_allowed = 3,
  scale_requires_precision = 4,
  scale_exceeds_precision = 5,
  length_required = 6,
  length_not_allowed = 7,
  vector_dimensions_required = 8,
  vector_dimensions_not_allowed = 9,
  container_rank_required = 10,
  container_rank_not_allowed = 11,
  range_subtype_required = 12,
  array_element_required = 13,
  charset_uuid_required = 14,
  collation_requires_character_family = 15,
  collation_requires_charset = 16,
  timezone_requires_temporal_family = 17,
  timezone_uuid_required = 18,
  domain_stack_without_domain_uuid = 19,
  domain_stack_current_uuid_mismatch = 20,
  element_descriptor_uuid_required = 21,
  reference_modifier_name_required = 22,
  reference_modifier_duplicate = 23
};

constexpr std::string_view ExecutionTypeModifierCanonicalizationStatusName(
    ExecutionTypeModifierCanonicalizationStatus status) noexcept {
  switch (status) {
    case ExecutionTypeModifierCanonicalizationStatus::ok:
      return "ok";
    case ExecutionTypeModifierCanonicalizationStatus::descriptor_invalid:
      return "descriptor_invalid";
    case ExecutionTypeModifierCanonicalizationStatus::precision_required:
      return "precision_required";
    case ExecutionTypeModifierCanonicalizationStatus::precision_not_allowed:
      return "precision_not_allowed";
    case ExecutionTypeModifierCanonicalizationStatus::scale_requires_precision:
      return "scale_requires_precision";
    case ExecutionTypeModifierCanonicalizationStatus::scale_exceeds_precision:
      return "scale_exceeds_precision";
    case ExecutionTypeModifierCanonicalizationStatus::length_required:
      return "length_required";
    case ExecutionTypeModifierCanonicalizationStatus::length_not_allowed:
      return "length_not_allowed";
    case ExecutionTypeModifierCanonicalizationStatus::
        vector_dimensions_required:
      return "vector_dimensions_required";
    case ExecutionTypeModifierCanonicalizationStatus::
        vector_dimensions_not_allowed:
      return "vector_dimensions_not_allowed";
    case ExecutionTypeModifierCanonicalizationStatus::container_rank_required:
      return "container_rank_required";
    case ExecutionTypeModifierCanonicalizationStatus::container_rank_not_allowed:
      return "container_rank_not_allowed";
    case ExecutionTypeModifierCanonicalizationStatus::range_subtype_required:
      return "range_subtype_required";
    case ExecutionTypeModifierCanonicalizationStatus::array_element_required:
      return "array_element_required";
    case ExecutionTypeModifierCanonicalizationStatus::charset_uuid_required:
      return "charset_uuid_required";
    case ExecutionTypeModifierCanonicalizationStatus::
        collation_requires_character_family:
      return "collation_requires_character_family";
    case ExecutionTypeModifierCanonicalizationStatus::collation_requires_charset:
      return "collation_requires_charset";
    case ExecutionTypeModifierCanonicalizationStatus::
        timezone_requires_temporal_family:
      return "timezone_requires_temporal_family";
    case ExecutionTypeModifierCanonicalizationStatus::timezone_uuid_required:
      return "timezone_uuid_required";
    case ExecutionTypeModifierCanonicalizationStatus::
        domain_stack_without_domain_uuid:
      return "domain_stack_without_domain_uuid";
    case ExecutionTypeModifierCanonicalizationStatus::
        domain_stack_current_uuid_mismatch:
      return "domain_stack_current_uuid_mismatch";
    case ExecutionTypeModifierCanonicalizationStatus::
        element_descriptor_uuid_required:
      return "element_descriptor_uuid_required";
    case ExecutionTypeModifierCanonicalizationStatus::reference_modifier_name_required:
      return "reference_modifier_name_required";
    case ExecutionTypeModifierCanonicalizationStatus::reference_modifier_duplicate:
      return "reference_modifier_duplicate";
  }
  return "unknown_status";
}

struct ExecutionTypeModifierCanonicalizationResult {
  ExecutionTypeModifierCanonicalizationStatus status =
      ExecutionTypeModifierCanonicalizationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  ExecutionTypeModifierCanonicalForm canonical_form;

  bool ok() const noexcept {
    return status == ExecutionTypeModifierCanonicalizationStatus::ok;
  }
};

constexpr bool ExecutionTypeFamilyAllowsPrecision(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::decimal;
}

constexpr bool ExecutionTypeFamilyAllowsLength(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::character ||
         family == ExecutionTypeFamily::binary ||
         family == ExecutionTypeFamily::bit_string ||
         family == ExecutionTypeFamily::blob;
}

constexpr bool ExecutionTypeFamilyAllowsContainerRank(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::structured ||
         family == ExecutionTypeFamily::document;
}

inline char ExecutionTypeModifierCanonicalLowerAscii(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

inline std::string ExecutionTypeModifierCanonicalToken(std::string_view value) {
  std::string token;
  bool last_was_separator = false;
  for (const char ch : value) {
    const bool separator = ch == ' ' || ch == '_' || ch == '-';
    if (separator) {
      if (!token.empty()) {
        last_was_separator = true;
      }
      continue;
    }
    if (last_was_separator && !token.empty()) {
      token.push_back('_');
    }
    token.push_back(ExecutionTypeModifierCanonicalLowerAscii(ch));
    last_was_separator = false;
  }
  return token;
}

inline std::string ExecutionTypeModifierCanonicalUuidToken(const Uuid& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(32);
  for (const std::uint8_t byte : uuid.bytes) {
    token.push_back(kHex[(byte >> 4) & 0x0f]);
    token.push_back(kHex[byte & 0x0f]);
  }
  return token;
}

inline void ExecutionTypeModifierAppendKey(std::string& key,
                                           std::string_view name,
                                           std::string_view value) {
  key.push_back(';');
  key.append(name);
  key.push_back('=');
  key.append(value);
}

inline void ExecutionTypeModifierAppendKey(std::string& key,
                                           std::string_view name,
                                           std::uint64_t value) {
  ExecutionTypeModifierAppendKey(key, name, std::to_string(value));
}

inline void ExecutionTypeModifierAppendUuidKey(std::string& key,
                                               std::string_view name,
                                               const Uuid& uuid) {
  ExecutionTypeModifierAppendKey(
      key, name, ExecutionTypeModifierCanonicalUuidToken(uuid));
}

inline ExecutionTypeModifierCanonicalizationResult
CanonicalizeExecutionTypeModifiers(
    const ExecutionTypeModifierCanonicalizationInput& input) {
  ExecutionTypeModifierCanonicalizationResult result;
  auto& form = result.canonical_form;
  form.descriptor = input.descriptor;

  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(form.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionTypeModifierCanonicalizationStatus::descriptor_invalid,
            descriptor_result.status, {}};
  }

  form.descriptor.modifier_flags = 0;
  if (form.descriptor.precision != 0) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::precision);
  }
  if (form.descriptor.scale != 0 || form.descriptor.precision != 0) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::scale);
  }
  if (form.descriptor.length != 0) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::length);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.charset_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::charset_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.collation_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::collation_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.timezone_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::timezone_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.domain_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::domain_uuid);
  }
  if (!form.descriptor.domain_stack.empty()) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::domain_stack);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.element_descriptor_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(
            ExecutionTypeModifierFlag::element_descriptor_uuid);
  }
  if (form.descriptor.vector_dimensions != 0) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(
            ExecutionTypeModifierFlag::vector_dimensions);
  }
  if (form.descriptor.container_rank != 0) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(ExecutionTypeModifierFlag::container_rank);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.security_policy_uuid)) {
    form.descriptor.modifier_flags |=
        ExecutionTypeModifierFlagBit(
            ExecutionTypeModifierFlag::security_policy_uuid);
  }

  if (ExecutionTypeFamilyAllowsPrecision(form.descriptor.family)) {
    if (form.descriptor.precision == 0) {
      return {ExecutionTypeModifierCanonicalizationStatus::precision_required,
              ExecutionDataPacketStatus::ok, {}};
    }
    if (form.descriptor.scale > form.descriptor.precision) {
      return {
          ExecutionTypeModifierCanonicalizationStatus::scale_exceeds_precision,
          ExecutionDataPacketStatus::ok, {}};
    }
  } else {
    if (form.descriptor.precision != 0) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  precision_not_allowed,
              ExecutionDataPacketStatus::ok, {}};
    }
    if (form.descriptor.scale != 0) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  scale_requires_precision,
              ExecutionDataPacketStatus::ok, {}};
    }
  }

  if (form.descriptor.scale != 0 && form.descriptor.precision == 0) {
    return {ExecutionTypeModifierCanonicalizationStatus::
                scale_requires_precision,
            ExecutionDataPacketStatus::ok, {}};
  }

  if (ExecutionTypeFamilyAllowsLength(form.descriptor.family)) {
    if (form.descriptor.family != ExecutionTypeFamily::blob &&
        form.descriptor.length == 0) {
      return {ExecutionTypeModifierCanonicalizationStatus::length_required,
              ExecutionDataPacketStatus::ok, {}};
    }
  } else if (form.descriptor.length != 0) {
    return {ExecutionTypeModifierCanonicalizationStatus::length_not_allowed,
            ExecutionDataPacketStatus::ok, {}};
  }

  if (form.descriptor.family == ExecutionTypeFamily::vector) {
    if (form.descriptor.vector_dimensions == 0) {
      return {
          ExecutionTypeModifierCanonicalizationStatus::vector_dimensions_required,
          ExecutionDataPacketStatus::ok, {}};
    }
  } else if (form.descriptor.vector_dimensions != 0) {
    return {
        ExecutionTypeModifierCanonicalizationStatus::vector_dimensions_not_allowed,
        ExecutionDataPacketStatus::ok, {}};
  }

  if (ExecutionTypeFamilyAllowsContainerRank(form.descriptor.family)) {
    if (form.descriptor.container_rank == 0) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  container_rank_required,
              ExecutionDataPacketStatus::ok, {}};
    }
    if (!input.array_element_present &&
        ExecutionDataPacketUuidIsNil(form.descriptor.element_descriptor_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  array_element_required,
              ExecutionDataPacketStatus::ok, {}};
    }
  } else if (form.descriptor.container_rank != 0) {
    return {ExecutionTypeModifierCanonicalizationStatus::
                container_rank_not_allowed,
            ExecutionDataPacketStatus::ok, {}};
  }

  if (form.descriptor.family == ExecutionTypeFamily::range &&
      !input.range_subtype_present &&
      ExecutionDataPacketUuidIsNil(form.descriptor.element_descriptor_uuid)) {
    return {ExecutionTypeModifierCanonicalizationStatus::range_subtype_required,
            ExecutionDataPacketStatus::ok, {}};
  }

  if (form.descriptor.family == ExecutionTypeFamily::character) {
    if (!ExecutionDataPacketUuidIsNil(form.descriptor.collation_uuid) &&
        ExecutionDataPacketUuidIsNil(form.descriptor.charset_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  collation_requires_charset,
              ExecutionDataPacketStatus::ok, {}};
    }
  } else {
    if (!ExecutionDataPacketUuidIsNil(form.descriptor.charset_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::charset_uuid_required,
              ExecutionDataPacketStatus::ok, {}};
    }
    if (!ExecutionDataPacketUuidIsNil(form.descriptor.collation_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  collation_requires_character_family,
              ExecutionDataPacketStatus::ok, {}};
    }
  }

  if (form.descriptor.family == ExecutionTypeFamily::temporal) {
    if (ExecutionDataPacketUuidIsNil(form.descriptor.timezone_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::timezone_uuid_required,
              ExecutionDataPacketStatus::ok, {}};
    }
  } else if (!ExecutionDataPacketUuidIsNil(form.descriptor.timezone_uuid)) {
    return {ExecutionTypeModifierCanonicalizationStatus::
                timezone_requires_temporal_family,
            ExecutionDataPacketStatus::ok, {}};
  }

  if (ExecutionDataPacketUuidIsNil(form.descriptor.domain_uuid) &&
      !form.descriptor.domain_stack.empty()) {
    return {ExecutionTypeModifierCanonicalizationStatus::
                domain_stack_without_domain_uuid,
            ExecutionDataPacketStatus::ok, {}};
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.domain_uuid)) {
    if (form.descriptor.domain_stack.empty()) {
      form.descriptor.domain_stack.push_back(form.descriptor.domain_uuid);
    }
    if (!ExecutionDataPacketUuidEquals(form.descriptor.domain_stack.front(),
                                      form.descriptor.domain_uuid)) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  domain_stack_current_uuid_mismatch,
              ExecutionDataPacketStatus::ok, {}};
    }
  }

  if ((form.descriptor.family == ExecutionTypeFamily::range ||
       ExecutionTypeFamilyAllowsContainerRank(form.descriptor.family)) &&
      ExecutionDataPacketUuidIsNil(form.descriptor.element_descriptor_uuid)) {
    return {ExecutionTypeModifierCanonicalizationStatus::
                element_descriptor_uuid_required,
            ExecutionDataPacketStatus::ok, {}};
  }

  form.reference_modifiers = input.reference_modifiers;
  for (auto& modifier : form.reference_modifiers) {
    modifier.name = ExecutionTypeModifierCanonicalToken(modifier.name);
    modifier.value = ExecutionTypeModifierCanonicalToken(modifier.value);
    if (modifier.name.empty()) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  reference_modifier_name_required,
              ExecutionDataPacketStatus::ok, {}};
    }
  }
  std::sort(form.reference_modifiers.begin(), form.reference_modifiers.end(),
            [](const ExecutionTypeReferenceModifier& left,
               const ExecutionTypeReferenceModifier& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.value < right.value;
            });
  for (std::size_t index = 1; index < form.reference_modifiers.size(); ++index) {
    if (form.reference_modifiers[index - 1].name ==
            form.reference_modifiers[index].name &&
        form.reference_modifiers[index - 1].value ==
            form.reference_modifiers[index].value) {
      return {ExecutionTypeModifierCanonicalizationStatus::
                  reference_modifier_duplicate,
              ExecutionDataPacketStatus::ok, {}};
    }
  }

  std::string key = "family:";
  key.append(std::to_string(static_cast<std::uint16_t>(form.descriptor.family)));
  ExecutionTypeModifierAppendKey(
      key, "width",
      static_cast<std::uint64_t>(form.descriptor.width_class));
  ExecutionTypeModifierAppendKey(key, "precision", form.descriptor.precision);
  ExecutionTypeModifierAppendKey(key, "scale", form.descriptor.scale);
  ExecutionTypeModifierAppendKey(key, "length", form.descriptor.length);
  ExecutionTypeModifierAppendKey(key, "vector",
                                 form.descriptor.vector_dimensions);
  ExecutionTypeModifierAppendKey(key, "rank", form.descriptor.container_rank);
  ExecutionTypeModifierAppendKey(key, "flags", form.descriptor.modifier_flags);
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.charset_uuid)) {
    ExecutionTypeModifierAppendUuidKey(key, "charset",
                                       form.descriptor.charset_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.collation_uuid)) {
    ExecutionTypeModifierAppendUuidKey(key, "collation",
                                       form.descriptor.collation_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.timezone_uuid)) {
    ExecutionTypeModifierAppendUuidKey(key, "timezone",
                                       form.descriptor.timezone_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.domain_uuid)) {
    ExecutionTypeModifierAppendUuidKey(key, "domain",
                                       form.descriptor.domain_uuid);
  }
  if (!ExecutionDataPacketUuidIsNil(form.descriptor.element_descriptor_uuid)) {
    ExecutionTypeModifierAppendUuidKey(
        key, "element", form.descriptor.element_descriptor_uuid);
  }
  for (const auto& modifier : form.reference_modifiers) {
    key.append(";reference:");
    key.append(modifier.name);
    key.push_back('=');
    key.append(modifier.value);
  }
  form.canonical_modifier_key = key;
  form.descriptor_identity_digest = key;
  return result;
}

// SEARCH_KEY: enum label UUID
// SEARCH_KEY: set domain
// SEARCH_KEY: unknown label policy
inline constexpr std::size_t kEnumSetRepresentationMaxLabels = 1024;
inline constexpr std::size_t kEnumSetBitsetStorageMaxLabels = 64;

enum class EnumSetRepresentationKind : std::uint8_t {
  enum_single = 0,
  set_collection = 1
};

constexpr bool EnumSetRepresentationKindIsValid(
    EnumSetRepresentationKind kind) noexcept {
  switch (kind) {
    case EnumSetRepresentationKind::enum_single:
    case EnumSetRepresentationKind::set_collection:
      return true;
  }
  return false;
}

enum class EnumSetStorageKind : std::uint8_t {
  ordinal = 0,
  bitset = 1,
  list = 2,
  string = 3,
  reference_native = 4
};

constexpr bool EnumSetStorageKindIsValid(EnumSetStorageKind kind) noexcept {
  switch (kind) {
    case EnumSetStorageKind::ordinal:
    case EnumSetStorageKind::bitset:
    case EnumSetStorageKind::list:
    case EnumSetStorageKind::string:
    case EnumSetStorageKind::reference_native:
      return true;
  }
  return false;
}

enum class EnumSetUnknownLabelPolicy : std::uint8_t {
  reject = 0,
  map_to_unknown_label = 1,
  reference_compatibility = 2
};

constexpr bool EnumSetUnknownLabelPolicyIsValid(
    EnumSetUnknownLabelPolicy policy) noexcept {
  switch (policy) {
    case EnumSetUnknownLabelPolicy::reject:
    case EnumSetUnknownLabelPolicy::map_to_unknown_label:
    case EnumSetUnknownLabelPolicy::reference_compatibility:
      return true;
  }
  return false;
}

struct EnumSetLabelDescriptor {
  Uuid label_uuid{};
  std::uint32_t ordinal = 0;
  std::string stable_name;
  std::string canonical_rendering;
  std::string reference_rendering_name;
  std::vector<std::string> aliases;
};

struct EnumSetRepresentationDescriptor {
  Uuid enum_set_uuid{};
  Uuid representation_descriptor_uuid{};
  Uuid domain_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  EnumSetRepresentationKind representation_kind =
      EnumSetRepresentationKind::enum_single;
  EnumSetStorageKind storage_kind = EnumSetStorageKind::ordinal;
  EnumSetUnknownLabelPolicy unknown_label_policy =
      EnumSetUnknownLabelPolicy::reject;
  std::vector<EnumSetLabelDescriptor> labels;
  Uuid unknown_label_uuid{};
  Uuid reference_profile_uuid{};
  bool ordered = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class EnumSetRepresentationStatus : std::uint8_t {
  ok = 0,
  enum_set_uuid_required = 1,
  representation_descriptor_uuid_required = 2,
  domain_uuid_required = 3,
  descriptor_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  representation_kind_invalid = 8,
  storage_kind_invalid = 9,
  unknown_label_policy_invalid = 10,
  labels_required = 11,
  label_count_exceeds_limit = 12,
  label_uuid_required = 13,
  label_stable_name_required = 14,
  label_rendering_required = 15,
  label_ordinal_mismatch = 16,
  label_uuid_duplicate = 17,
  label_name_duplicate = 18,
  label_alias_duplicate = 19,
  bitset_label_count_exceeds_limit = 20,
  enum_storage_kind_invalid = 21,
  set_storage_kind_invalid = 22,
  reference_rendering_required = 23,
  reference_profile_uuid_required = 24,
  unknown_label_uuid_required = 25,
  unknown_label_uuid_not_found = 26,
  unordered_set_requires_list_storage = 27
};

constexpr std::string_view EnumSetRepresentationStatusName(
    EnumSetRepresentationStatus status) noexcept {
  switch (status) {
    case EnumSetRepresentationStatus::ok:
      return "ok";
    case EnumSetRepresentationStatus::enum_set_uuid_required:
      return "enum_set_uuid_required";
    case EnumSetRepresentationStatus::representation_descriptor_uuid_required:
      return "representation_descriptor_uuid_required";
    case EnumSetRepresentationStatus::domain_uuid_required:
      return "domain_uuid_required";
    case EnumSetRepresentationStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case EnumSetRepresentationStatus::stable_name_required:
      return "stable_name_required";
    case EnumSetRepresentationStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case EnumSetRepresentationStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case EnumSetRepresentationStatus::representation_kind_invalid:
      return "representation_kind_invalid";
    case EnumSetRepresentationStatus::storage_kind_invalid:
      return "storage_kind_invalid";
    case EnumSetRepresentationStatus::unknown_label_policy_invalid:
      return "unknown_label_policy_invalid";
    case EnumSetRepresentationStatus::labels_required:
      return "labels_required";
    case EnumSetRepresentationStatus::label_count_exceeds_limit:
      return "label_count_exceeds_limit";
    case EnumSetRepresentationStatus::label_uuid_required:
      return "label_uuid_required";
    case EnumSetRepresentationStatus::label_stable_name_required:
      return "label_stable_name_required";
    case EnumSetRepresentationStatus::label_rendering_required:
      return "label_rendering_required";
    case EnumSetRepresentationStatus::label_ordinal_mismatch:
      return "label_ordinal_mismatch";
    case EnumSetRepresentationStatus::label_uuid_duplicate:
      return "label_uuid_duplicate";
    case EnumSetRepresentationStatus::label_name_duplicate:
      return "label_name_duplicate";
    case EnumSetRepresentationStatus::label_alias_duplicate:
      return "label_alias_duplicate";
    case EnumSetRepresentationStatus::bitset_label_count_exceeds_limit:
      return "bitset_label_count_exceeds_limit";
    case EnumSetRepresentationStatus::enum_storage_kind_invalid:
      return "enum_storage_kind_invalid";
    case EnumSetRepresentationStatus::set_storage_kind_invalid:
      return "set_storage_kind_invalid";
    case EnumSetRepresentationStatus::reference_rendering_required:
      return "reference_rendering_required";
    case EnumSetRepresentationStatus::reference_profile_uuid_required:
      return "reference_profile_uuid_required";
    case EnumSetRepresentationStatus::unknown_label_uuid_required:
      return "unknown_label_uuid_required";
    case EnumSetRepresentationStatus::unknown_label_uuid_not_found:
      return "unknown_label_uuid_not_found";
    case EnumSetRepresentationStatus::unordered_set_requires_list_storage:
      return "unordered_set_requires_list_storage";
  }
  return "unknown_status";
}

struct EnumSetRepresentationValidationResult {
  EnumSetRepresentationStatus status = EnumSetRepresentationStatus::ok;
  std::size_t label_index = 0;

  bool ok() const noexcept {
    return status == EnumSetRepresentationStatus::ok;
  }
};

inline bool EnumSetRenderingPolicyRequiresReference(
    const EnumSetRepresentationDescriptor& descriptor) noexcept {
  return descriptor.storage_kind == EnumSetStorageKind::reference_native ||
         descriptor.unknown_label_policy ==
             EnumSetUnknownLabelPolicy::reference_compatibility;
}

inline bool EnumSetHasLabelUuid(
    const EnumSetRepresentationDescriptor& descriptor,
    const Uuid& label_uuid) {
  for (const auto& label : descriptor.labels) {
    if (ExecutionDataPacketUuidEquals(label.label_uuid, label_uuid)) {
      return true;
    }
  }
  return false;
}

inline EnumSetRepresentationValidationResult
ValidateEnumSetRepresentationDescriptor(
    const EnumSetRepresentationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.enum_set_uuid)) {
    return {EnumSetRepresentationStatus::enum_set_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(
          descriptor.representation_descriptor_uuid)) {
    return {
        EnumSetRepresentationStatus::representation_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.domain_uuid)) {
    return {EnumSetRepresentationStatus::domain_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {EnumSetRepresentationStatus::descriptor_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {EnumSetRepresentationStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {EnumSetRepresentationStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {EnumSetRepresentationStatus::descriptor_parser_dependent};
  }
  if (!EnumSetRepresentationKindIsValid(descriptor.representation_kind)) {
    return {EnumSetRepresentationStatus::representation_kind_invalid};
  }
  if (!EnumSetStorageKindIsValid(descriptor.storage_kind)) {
    return {EnumSetRepresentationStatus::storage_kind_invalid};
  }
  if (!EnumSetUnknownLabelPolicyIsValid(descriptor.unknown_label_policy)) {
    return {EnumSetRepresentationStatus::unknown_label_policy_invalid};
  }
  if (descriptor.labels.empty()) {
    return {EnumSetRepresentationStatus::labels_required};
  }
  if (descriptor.labels.size() > kEnumSetRepresentationMaxLabels) {
    return {EnumSetRepresentationStatus::label_count_exceeds_limit};
  }
  if (descriptor.storage_kind == EnumSetStorageKind::bitset &&
      descriptor.labels.size() > kEnumSetBitsetStorageMaxLabels) {
    return {EnumSetRepresentationStatus::bitset_label_count_exceeds_limit};
  }
  if (descriptor.representation_kind == EnumSetRepresentationKind::enum_single &&
      (descriptor.storage_kind == EnumSetStorageKind::bitset ||
       descriptor.storage_kind == EnumSetStorageKind::list)) {
    return {EnumSetRepresentationStatus::enum_storage_kind_invalid};
  }
  if (descriptor.representation_kind ==
          EnumSetRepresentationKind::set_collection &&
      descriptor.storage_kind == EnumSetStorageKind::ordinal) {
    return {EnumSetRepresentationStatus::set_storage_kind_invalid};
  }
  if (descriptor.representation_kind ==
          EnumSetRepresentationKind::set_collection &&
      !descriptor.ordered && descriptor.storage_kind != EnumSetStorageKind::list) {
    return {EnumSetRepresentationStatus::unordered_set_requires_list_storage};
  }
  if (EnumSetRenderingPolicyRequiresReference(descriptor) &&
      ExecutionDataPacketUuidIsNil(descriptor.reference_profile_uuid)) {
    return {EnumSetRepresentationStatus::reference_profile_uuid_required};
  }
  if (descriptor.unknown_label_policy ==
          EnumSetUnknownLabelPolicy::map_to_unknown_label &&
      ExecutionDataPacketUuidIsNil(descriptor.unknown_label_uuid)) {
    return {EnumSetRepresentationStatus::unknown_label_uuid_required};
  }

  std::vector<Uuid> label_uuids;
  std::vector<std::string> label_tokens;
  label_uuids.reserve(descriptor.labels.size());
  label_tokens.reserve(descriptor.labels.size() * 2);
  for (std::size_t index = 0; index < descriptor.labels.size(); ++index) {
    const auto& label = descriptor.labels[index];
    if (ExecutionDataPacketUuidIsNil(label.label_uuid)) {
      return {EnumSetRepresentationStatus::label_uuid_required, index};
    }
    if (label.stable_name.empty()) {
      return {EnumSetRepresentationStatus::label_stable_name_required, index};
    }
    if (label.canonical_rendering.empty()) {
      return {EnumSetRepresentationStatus::label_rendering_required, index};
    }
    if (EnumSetRenderingPolicyRequiresReference(descriptor) &&
        label.reference_rendering_name.empty()) {
      return {EnumSetRepresentationStatus::reference_rendering_required, index};
    }
    if (label.ordinal != index) {
      return {EnumSetRepresentationStatus::label_ordinal_mismatch, index};
    }
    for (const auto& seen_uuid : label_uuids) {
      if (ExecutionDataPacketUuidEquals(seen_uuid, label.label_uuid)) {
        return {EnumSetRepresentationStatus::label_uuid_duplicate, index};
      }
    }
    label_uuids.push_back(label.label_uuid);

    auto stable_token = ExecutionTypeModifierCanonicalToken(label.stable_name);
    for (const auto& seen_token : label_tokens) {
      if (seen_token == stable_token) {
        return {EnumSetRepresentationStatus::label_name_duplicate, index};
      }
    }
    label_tokens.push_back(stable_token);
    for (const auto& alias : label.aliases) {
      auto alias_token = ExecutionTypeModifierCanonicalToken(alias);
      if (alias_token.empty()) {
        return {EnumSetRepresentationStatus::label_alias_duplicate, index};
      }
      for (const auto& seen_token : label_tokens) {
        if (seen_token == alias_token) {
          return {EnumSetRepresentationStatus::label_alias_duplicate, index};
        }
      }
      label_tokens.push_back(alias_token);
    }
  }

  if (descriptor.unknown_label_policy ==
          EnumSetUnknownLabelPolicy::map_to_unknown_label &&
      !EnumSetHasLabelUuid(descriptor, descriptor.unknown_label_uuid)) {
    return {EnumSetRepresentationStatus::unknown_label_uuid_not_found};
  }
  return {};
}

// SEARCH_KEY: empty range
// SEARCH_KEY: multirange
// SEARCH_KEY: bound inclusion
inline constexpr std::size_t kRangeRepresentationMaxSegments = 1024;

enum class RangeRepresentationKind : std::uint8_t {
  single_range = 0,
  multirange = 1
};

constexpr bool RangeRepresentationKindIsValid(
    RangeRepresentationKind kind) noexcept {
  switch (kind) {
    case RangeRepresentationKind::single_range:
    case RangeRepresentationKind::multirange:
      return true;
  }
  return false;
}

enum class RangeBoundKind : std::uint8_t {
  finite = 0,
  unbounded = 1
};

constexpr bool RangeBoundKindIsValid(RangeBoundKind kind) noexcept {
  switch (kind) {
    case RangeBoundKind::finite:
    case RangeBoundKind::unbounded:
      return true;
  }
  return false;
}

struct RangeBoundDescriptor {
  RangeBoundKind bound_kind = RangeBoundKind::finite;
  bool inclusive = true;
  ExecutionValueState value_state = ExecutionValueState::value;
  std::vector<std::uint8_t> payload;
  std::string canonical_token;
};

struct RangeSegmentDescriptor {
  bool empty = false;
  RangeBoundDescriptor lower_bound;
  RangeBoundDescriptor upper_bound;
};

struct RangeRepresentationDescriptor {
  Uuid range_uuid{};
  Uuid representation_descriptor_uuid{};
  Uuid subtype_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  RangeRepresentationKind representation_kind =
      RangeRepresentationKind::single_range;
  ExecutionTypeDescriptor subtype_descriptor;
  std::vector<RangeSegmentDescriptor> segments;
  bool canonicalized = true;
  bool empty_distinct_from_null = true;
  bool empty_distinct_from_missing = true;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class RangeRepresentationStatus : std::uint8_t {
  ok = 0,
  range_uuid_required = 1,
  representation_descriptor_uuid_required = 2,
  subtype_descriptor_uuid_required = 3,
  descriptor_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  representation_kind_invalid = 8,
  subtype_descriptor_invalid = 9,
  subtype_descriptor_uuid_mismatch = 10,
  segments_required = 11,
  segment_count_exceeds_limit = 12,
  single_range_segment_count_invalid = 13,
  bound_kind_invalid = 14,
  finite_bound_payload_required = 15,
  finite_bound_token_required = 16,
  unbounded_bound_payload_not_allowed = 17,
  unbounded_bound_inclusive_not_allowed = 18,
  empty_segment_bound_payload_not_allowed = 19,
  empty_range_not_distinct = 20,
  canonicalization_required = 21,
  segment_order_invalid = 22,
  segment_overlap_or_adjacency = 23,
  bound_value_state_invalid = 24,
  lower_bound_required = 25,
  upper_bound_required = 26
};

constexpr std::string_view RangeRepresentationStatusName(
    RangeRepresentationStatus status) noexcept {
  switch (status) {
    case RangeRepresentationStatus::ok:
      return "ok";
    case RangeRepresentationStatus::range_uuid_required:
      return "range_uuid_required";
    case RangeRepresentationStatus::representation_descriptor_uuid_required:
      return "representation_descriptor_uuid_required";
    case RangeRepresentationStatus::subtype_descriptor_uuid_required:
      return "subtype_descriptor_uuid_required";
    case RangeRepresentationStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case RangeRepresentationStatus::stable_name_required:
      return "stable_name_required";
    case RangeRepresentationStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case RangeRepresentationStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case RangeRepresentationStatus::representation_kind_invalid:
      return "representation_kind_invalid";
    case RangeRepresentationStatus::subtype_descriptor_invalid:
      return "subtype_descriptor_invalid";
    case RangeRepresentationStatus::subtype_descriptor_uuid_mismatch:
      return "subtype_descriptor_uuid_mismatch";
    case RangeRepresentationStatus::segments_required:
      return "segments_required";
    case RangeRepresentationStatus::segment_count_exceeds_limit:
      return "segment_count_exceeds_limit";
    case RangeRepresentationStatus::single_range_segment_count_invalid:
      return "single_range_segment_count_invalid";
    case RangeRepresentationStatus::bound_kind_invalid:
      return "bound_kind_invalid";
    case RangeRepresentationStatus::finite_bound_payload_required:
      return "finite_bound_payload_required";
    case RangeRepresentationStatus::finite_bound_token_required:
      return "finite_bound_token_required";
    case RangeRepresentationStatus::unbounded_bound_payload_not_allowed:
      return "unbounded_bound_payload_not_allowed";
    case RangeRepresentationStatus::unbounded_bound_inclusive_not_allowed:
      return "unbounded_bound_inclusive_not_allowed";
    case RangeRepresentationStatus::empty_segment_bound_payload_not_allowed:
      return "empty_segment_bound_payload_not_allowed";
    case RangeRepresentationStatus::empty_range_not_distinct:
      return "empty_range_not_distinct";
    case RangeRepresentationStatus::canonicalization_required:
      return "canonicalization_required";
    case RangeRepresentationStatus::segment_order_invalid:
      return "segment_order_invalid";
    case RangeRepresentationStatus::segment_overlap_or_adjacency:
      return "segment_overlap_or_adjacency";
    case RangeRepresentationStatus::bound_value_state_invalid:
      return "bound_value_state_invalid";
    case RangeRepresentationStatus::lower_bound_required:
      return "lower_bound_required";
    case RangeRepresentationStatus::upper_bound_required:
      return "upper_bound_required";
  }
  return "unknown_status";
}

struct RangeRepresentationValidationResult {
  RangeRepresentationStatus status = RangeRepresentationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t segment_index = 0;

  bool ok() const noexcept {
    return status == RangeRepresentationStatus::ok;
  }
};

inline RangeRepresentationStatus ValidateRangeBoundDescriptor(
    const RangeBoundDescriptor& bound) {
  if (!RangeBoundKindIsValid(bound.bound_kind)) {
    return RangeRepresentationStatus::bound_kind_invalid;
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(bound.value_state)) ||
      (bound.bound_kind == RangeBoundKind::finite &&
       bound.value_state != ExecutionValueState::value)) {
    return RangeRepresentationStatus::bound_value_state_invalid;
  }
  if (bound.bound_kind == RangeBoundKind::finite) {
    if (bound.payload.empty()) {
      return RangeRepresentationStatus::finite_bound_payload_required;
    }
    if (bound.canonical_token.empty()) {
      return RangeRepresentationStatus::finite_bound_token_required;
    }
  } else {
    if (!bound.payload.empty() || !bound.canonical_token.empty()) {
      return RangeRepresentationStatus::unbounded_bound_payload_not_allowed;
    }
    if (bound.inclusive) {
      return RangeRepresentationStatus::unbounded_bound_inclusive_not_allowed;
    }
  }
  return RangeRepresentationStatus::ok;
}

inline int RangeBoundTokenCompare(const std::string& left,
                                  const std::string& right) {
  if (left < right) {
    return -1;
  }
  if (right < left) {
    return 1;
  }
  return 0;
}

inline RangeRepresentationValidationResult ValidateRangeRepresentationDescriptor(
    const RangeRepresentationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.range_uuid)) {
    return {RangeRepresentationStatus::range_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(
          descriptor.representation_descriptor_uuid)) {
    return {
        RangeRepresentationStatus::representation_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.subtype_descriptor_uuid)) {
    return {RangeRepresentationStatus::subtype_descriptor_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {RangeRepresentationStatus::descriptor_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {RangeRepresentationStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {RangeRepresentationStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {RangeRepresentationStatus::descriptor_parser_dependent};
  }
  if (!RangeRepresentationKindIsValid(descriptor.representation_kind)) {
    return {RangeRepresentationStatus::representation_kind_invalid};
  }
  const auto subtype_result =
      ValidateExecutionDataPacketDescriptor(descriptor.subtype_descriptor, 0);
  if (!subtype_result.ok()) {
    return {RangeRepresentationStatus::subtype_descriptor_invalid,
            subtype_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.subtype_descriptor.descriptor_uuid,
          descriptor.subtype_descriptor_uuid)) {
    return {RangeRepresentationStatus::subtype_descriptor_uuid_mismatch};
  }
  if (descriptor.segments.empty()) {
    return {RangeRepresentationStatus::segments_required};
  }
  if (descriptor.segments.size() > kRangeRepresentationMaxSegments) {
    return {RangeRepresentationStatus::segment_count_exceeds_limit};
  }
  if (descriptor.representation_kind == RangeRepresentationKind::single_range &&
      descriptor.segments.size() != 1) {
    return {RangeRepresentationStatus::single_range_segment_count_invalid};
  }
  if (!descriptor.empty_distinct_from_null ||
      !descriptor.empty_distinct_from_missing) {
    return {RangeRepresentationStatus::empty_range_not_distinct};
  }
  if (!descriptor.canonicalized) {
    return {RangeRepresentationStatus::canonicalization_required};
  }

  const RangeSegmentDescriptor* previous = nullptr;
  for (std::size_t index = 0; index < descriptor.segments.size(); ++index) {
    const auto& segment = descriptor.segments[index];
    if (segment.empty) {
      if (!segment.lower_bound.payload.empty() ||
          !segment.upper_bound.payload.empty() ||
          !segment.lower_bound.canonical_token.empty() ||
          !segment.upper_bound.canonical_token.empty()) {
        return {RangeRepresentationStatus::
                    empty_segment_bound_payload_not_allowed,
                ExecutionDataPacketStatus::ok, index};
      }
      if (descriptor.segments.size() != 1) {
        return {RangeRepresentationStatus::segment_order_invalid,
                ExecutionDataPacketStatus::ok, index};
      }
      continue;
    }

    auto status = ValidateRangeBoundDescriptor(segment.lower_bound);
    if (status != RangeRepresentationStatus::ok) {
      return {status, ExecutionDataPacketStatus::ok, index};
    }
    status = ValidateRangeBoundDescriptor(segment.upper_bound);
    if (status != RangeRepresentationStatus::ok) {
      return {status, ExecutionDataPacketStatus::ok, index};
    }
    if (segment.lower_bound.bound_kind == RangeBoundKind::unbounded &&
        segment.upper_bound.bound_kind == RangeBoundKind::unbounded) {
      previous = &segment;
      continue;
    }
    if (segment.lower_bound.bound_kind == RangeBoundKind::finite &&
        segment.upper_bound.bound_kind == RangeBoundKind::finite) {
      const int comparison = RangeBoundTokenCompare(
          segment.lower_bound.canonical_token,
          segment.upper_bound.canonical_token);
      if (comparison > 0 ||
          (comparison == 0 &&
           (segment.lower_bound.inclusive || segment.upper_bound.inclusive))) {
        return {RangeRepresentationStatus::segment_order_invalid,
                ExecutionDataPacketStatus::ok, index};
      }
    }
    if (previous != nullptr && !previous->empty) {
      if (previous->upper_bound.bound_kind == RangeBoundKind::unbounded ||
          segment.lower_bound.bound_kind == RangeBoundKind::unbounded) {
        return {RangeRepresentationStatus::segment_order_invalid,
                ExecutionDataPacketStatus::ok, index};
      }
      const int comparison = RangeBoundTokenCompare(
          previous->upper_bound.canonical_token,
          segment.lower_bound.canonical_token);
      if (comparison > 0) {
        return {RangeRepresentationStatus::segment_order_invalid,
                ExecutionDataPacketStatus::ok, index};
      }
      if (comparison == 0 &&
          (previous->upper_bound.inclusive || segment.lower_bound.inclusive)) {
        return {RangeRepresentationStatus::segment_overlap_or_adjacency,
                ExecutionDataPacketStatus::ok, index};
      }
    }
    previous = &segment;
  }
  return {};
}

// SEARCH_KEY: lower bounds
// SEARCH_KEY: null bitmap
// SEARCH_KEY: sparse layout
// SEARCH_KEY: map key descriptor
inline constexpr std::size_t kContainerRepresentationMaxDimensions = 16;
inline constexpr std::size_t kContainerRepresentationMaxFields = 1024;

enum class ContainerRepresentationKind : std::uint8_t {
  array = 0,
  list = 1,
  map = 2,
  row = 3,
  composite = 4
};

constexpr bool ContainerRepresentationKindIsValid(
    ContainerRepresentationKind kind) noexcept {
  switch (kind) {
    case ContainerRepresentationKind::array:
    case ContainerRepresentationKind::list:
    case ContainerRepresentationKind::map:
    case ContainerRepresentationKind::row:
    case ContainerRepresentationKind::composite:
      return true;
  }
  return false;
}

enum class ContainerStorageKind : std::uint8_t {
  dense = 0,
  sparse = 1,
  ordered_map = 2,
  hash_map = 3,
  row_tuple = 4
};

constexpr bool ContainerStorageKindIsValid(ContainerStorageKind kind) noexcept {
  switch (kind) {
    case ContainerStorageKind::dense:
    case ContainerStorageKind::sparse:
    case ContainerStorageKind::ordered_map:
    case ContainerStorageKind::hash_map:
    case ContainerStorageKind::row_tuple:
      return true;
  }
  return false;
}

struct ContainerDimensionDescriptor {
  std::int64_t lower_bound = 0;
  std::uint64_t length = 0;
};

struct ContainerFieldDescriptor {
  Uuid field_uuid{};
  std::uint32_t ordinal = 0;
  std::string stable_name;
  Uuid element_descriptor_uuid{};
  ExecutionTypeDescriptor element_descriptor;
  bool nullable = true;
};

struct ContainerRepresentationDescriptor {
  Uuid container_uuid{};
  Uuid representation_descriptor_uuid{};
  Uuid element_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  ContainerRepresentationKind representation_kind =
      ContainerRepresentationKind::array;
  ContainerStorageKind storage_kind = ContainerStorageKind::dense;
  ExecutionTypeDescriptor element_descriptor;
  Uuid map_key_descriptor_uuid{};
  ExecutionTypeDescriptor map_key_descriptor;
  std::vector<ContainerDimensionDescriptor> dimensions;
  std::vector<ContainerFieldDescriptor> fields;
  std::uint64_t element_count = 0;
  bool ordered = true;
  bool sparse = false;
  bool null_bitmap_present = false;
  std::uint64_t null_bitmap_bits = 0;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class ContainerRepresentationStatus : std::uint8_t {
  ok = 0,
  container_uuid_required = 1,
  representation_descriptor_uuid_required = 2,
  element_descriptor_uuid_required = 3,
  descriptor_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  representation_kind_invalid = 8,
  storage_kind_invalid = 9,
  element_descriptor_invalid = 10,
  element_descriptor_uuid_mismatch = 11,
  dimensions_required = 12,
  dimension_count_exceeds_limit = 13,
  dimension_length_required = 14,
  list_dimension_invalid = 15,
  element_count_required = 16,
  storage_kind_incompatible = 17,
  sparse_flag_required = 18,
  sparse_flag_not_allowed = 19,
  null_bitmap_required = 20,
  null_bitmap_not_allowed = 21,
  null_bitmap_size_mismatch = 22,
  map_key_descriptor_uuid_required = 23,
  map_key_descriptor_invalid = 24,
  map_key_descriptor_uuid_mismatch = 25,
  map_key_nullable_not_allowed = 26,
  fields_required = 27,
  field_count_exceeds_limit = 28,
  field_uuid_required = 29,
  field_stable_name_required = 30,
  field_ordinal_mismatch = 31,
  field_uuid_duplicate = 32,
  field_name_duplicate = 33,
  field_descriptor_uuid_required = 34,
  field_descriptor_invalid = 35,
  field_descriptor_uuid_mismatch = 36,
  dimensions_not_allowed = 37,
  fields_not_allowed = 38,
  map_key_descriptor_not_allowed = 39
};

constexpr std::string_view ContainerRepresentationStatusName(
    ContainerRepresentationStatus status) noexcept {
  switch (status) {
    case ContainerRepresentationStatus::ok:
      return "ok";
    case ContainerRepresentationStatus::container_uuid_required:
      return "container_uuid_required";
    case ContainerRepresentationStatus::representation_descriptor_uuid_required:
      return "representation_descriptor_uuid_required";
    case ContainerRepresentationStatus::element_descriptor_uuid_required:
      return "element_descriptor_uuid_required";
    case ContainerRepresentationStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case ContainerRepresentationStatus::stable_name_required:
      return "stable_name_required";
    case ContainerRepresentationStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case ContainerRepresentationStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case ContainerRepresentationStatus::representation_kind_invalid:
      return "representation_kind_invalid";
    case ContainerRepresentationStatus::storage_kind_invalid:
      return "storage_kind_invalid";
    case ContainerRepresentationStatus::element_descriptor_invalid:
      return "element_descriptor_invalid";
    case ContainerRepresentationStatus::element_descriptor_uuid_mismatch:
      return "element_descriptor_uuid_mismatch";
    case ContainerRepresentationStatus::dimensions_required:
      return "dimensions_required";
    case ContainerRepresentationStatus::dimension_count_exceeds_limit:
      return "dimension_count_exceeds_limit";
    case ContainerRepresentationStatus::dimension_length_required:
      return "dimension_length_required";
    case ContainerRepresentationStatus::list_dimension_invalid:
      return "list_dimension_invalid";
    case ContainerRepresentationStatus::element_count_required:
      return "element_count_required";
    case ContainerRepresentationStatus::storage_kind_incompatible:
      return "storage_kind_incompatible";
    case ContainerRepresentationStatus::sparse_flag_required:
      return "sparse_flag_required";
    case ContainerRepresentationStatus::sparse_flag_not_allowed:
      return "sparse_flag_not_allowed";
    case ContainerRepresentationStatus::null_bitmap_required:
      return "null_bitmap_required";
    case ContainerRepresentationStatus::null_bitmap_not_allowed:
      return "null_bitmap_not_allowed";
    case ContainerRepresentationStatus::null_bitmap_size_mismatch:
      return "null_bitmap_size_mismatch";
    case ContainerRepresentationStatus::map_key_descriptor_uuid_required:
      return "map_key_descriptor_uuid_required";
    case ContainerRepresentationStatus::map_key_descriptor_invalid:
      return "map_key_descriptor_invalid";
    case ContainerRepresentationStatus::map_key_descriptor_uuid_mismatch:
      return "map_key_descriptor_uuid_mismatch";
    case ContainerRepresentationStatus::map_key_nullable_not_allowed:
      return "map_key_nullable_not_allowed";
    case ContainerRepresentationStatus::fields_required:
      return "fields_required";
    case ContainerRepresentationStatus::field_count_exceeds_limit:
      return "field_count_exceeds_limit";
    case ContainerRepresentationStatus::field_uuid_required:
      return "field_uuid_required";
    case ContainerRepresentationStatus::field_stable_name_required:
      return "field_stable_name_required";
    case ContainerRepresentationStatus::field_ordinal_mismatch:
      return "field_ordinal_mismatch";
    case ContainerRepresentationStatus::field_uuid_duplicate:
      return "field_uuid_duplicate";
    case ContainerRepresentationStatus::field_name_duplicate:
      return "field_name_duplicate";
    case ContainerRepresentationStatus::field_descriptor_uuid_required:
      return "field_descriptor_uuid_required";
    case ContainerRepresentationStatus::field_descriptor_invalid:
      return "field_descriptor_invalid";
    case ContainerRepresentationStatus::field_descriptor_uuid_mismatch:
      return "field_descriptor_uuid_mismatch";
    case ContainerRepresentationStatus::dimensions_not_allowed:
      return "dimensions_not_allowed";
    case ContainerRepresentationStatus::fields_not_allowed:
      return "fields_not_allowed";
    case ContainerRepresentationStatus::map_key_descriptor_not_allowed:
      return "map_key_descriptor_not_allowed";
  }
  return "unknown_status";
}

struct ContainerRepresentationValidationResult {
  ContainerRepresentationStatus status = ContainerRepresentationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t field_index = 0;

  bool ok() const noexcept {
    return status == ContainerRepresentationStatus::ok;
  }
};

inline std::uint64_t ContainerDimensionElementCapacity(
    const std::vector<ContainerDimensionDescriptor>& dimensions) {
  std::uint64_t capacity = 1;
  for (const auto& dimension : dimensions) {
    if (dimension.length == 0 ||
        capacity > std::numeric_limits<std::uint64_t>::max() /
                       dimension.length) {
      return 0;
    }
    capacity *= dimension.length;
  }
  return capacity;
}

inline ContainerRepresentationValidationResult
ValidateContainerRepresentationDescriptor(
    const ContainerRepresentationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.container_uuid)) {
    return {ContainerRepresentationStatus::container_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(
          descriptor.representation_descriptor_uuid)) {
    return {
        ContainerRepresentationStatus::representation_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.element_descriptor_uuid)) {
    return {ContainerRepresentationStatus::element_descriptor_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {ContainerRepresentationStatus::descriptor_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {ContainerRepresentationStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {ContainerRepresentationStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {ContainerRepresentationStatus::descriptor_parser_dependent};
  }
  if (!ContainerRepresentationKindIsValid(descriptor.representation_kind)) {
    return {ContainerRepresentationStatus::representation_kind_invalid};
  }
  if (!ContainerStorageKindIsValid(descriptor.storage_kind)) {
    return {ContainerRepresentationStatus::storage_kind_invalid};
  }
  const auto element_result =
      ValidateExecutionDataPacketDescriptor(descriptor.element_descriptor, 0);
  if (!element_result.ok()) {
    return {ContainerRepresentationStatus::element_descriptor_invalid,
            element_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.element_descriptor.descriptor_uuid,
          descriptor.element_descriptor_uuid)) {
    return {ContainerRepresentationStatus::element_descriptor_uuid_mismatch};
  }

  const bool dimensional =
      descriptor.representation_kind == ContainerRepresentationKind::array ||
      descriptor.representation_kind == ContainerRepresentationKind::list;
  const bool map =
      descriptor.representation_kind == ContainerRepresentationKind::map;
  const bool field_based =
      descriptor.representation_kind == ContainerRepresentationKind::row ||
      descriptor.representation_kind == ContainerRepresentationKind::composite;

  if (dimensional) {
    if (descriptor.dimensions.empty()) {
      return {ContainerRepresentationStatus::dimensions_required};
    }
    if (descriptor.dimensions.size() > kContainerRepresentationMaxDimensions) {
      return {ContainerRepresentationStatus::dimension_count_exceeds_limit};
    }
    for (const auto& dimension : descriptor.dimensions) {
      if (dimension.length == 0) {
        return {ContainerRepresentationStatus::dimension_length_required};
      }
    }
    if (descriptor.representation_kind == ContainerRepresentationKind::list &&
        (descriptor.dimensions.size() != 1 ||
         descriptor.dimensions.front().lower_bound != 0)) {
      return {ContainerRepresentationStatus::list_dimension_invalid};
    }
    if (descriptor.element_count == 0) {
      return {ContainerRepresentationStatus::element_count_required};
    }
    if (descriptor.storage_kind != ContainerStorageKind::dense &&
        descriptor.storage_kind != ContainerStorageKind::sparse) {
      return {ContainerRepresentationStatus::storage_kind_incompatible};
    }
    if (descriptor.storage_kind == ContainerStorageKind::sparse &&
        !descriptor.sparse) {
      return {ContainerRepresentationStatus::sparse_flag_required};
    }
    if (descriptor.storage_kind == ContainerStorageKind::dense &&
        descriptor.sparse) {
      return {ContainerRepresentationStatus::sparse_flag_not_allowed};
    }
    if (descriptor.element_descriptor.nullable_allowed &&
        !descriptor.null_bitmap_present) {
      return {ContainerRepresentationStatus::null_bitmap_required};
    }
    if (!descriptor.element_descriptor.nullable_allowed &&
        descriptor.null_bitmap_present) {
      return {ContainerRepresentationStatus::null_bitmap_not_allowed};
    }
    if (descriptor.null_bitmap_present &&
        descriptor.null_bitmap_bits != descriptor.element_count) {
      return {ContainerRepresentationStatus::null_bitmap_size_mismatch};
    }
  } else if (!descriptor.dimensions.empty()) {
    return {ContainerRepresentationStatus::dimensions_not_allowed};
  }

  if (map) {
    if (descriptor.storage_kind != ContainerStorageKind::ordered_map &&
        descriptor.storage_kind != ContainerStorageKind::hash_map) {
      return {ContainerRepresentationStatus::storage_kind_incompatible};
    }
    if (ExecutionDataPacketUuidIsNil(descriptor.map_key_descriptor_uuid)) {
      return {ContainerRepresentationStatus::map_key_descriptor_uuid_required};
    }
    const auto key_result =
        ValidateExecutionDataPacketDescriptor(descriptor.map_key_descriptor, 1);
    if (!key_result.ok()) {
      return {ContainerRepresentationStatus::map_key_descriptor_invalid,
              key_result.status};
    }
    if (!ExecutionDataPacketUuidEquals(
            descriptor.map_key_descriptor.descriptor_uuid,
            descriptor.map_key_descriptor_uuid)) {
      return {ContainerRepresentationStatus::map_key_descriptor_uuid_mismatch};
    }
    if (descriptor.map_key_descriptor.nullable_allowed) {
      return {ContainerRepresentationStatus::map_key_nullable_not_allowed};
    }
    if (descriptor.element_count == 0) {
      return {ContainerRepresentationStatus::element_count_required};
    }
    if (descriptor.element_descriptor.nullable_allowed &&
        !descriptor.null_bitmap_present) {
      return {ContainerRepresentationStatus::null_bitmap_required};
    }
    if (descriptor.null_bitmap_present &&
        descriptor.null_bitmap_bits != descriptor.element_count) {
      return {ContainerRepresentationStatus::null_bitmap_size_mismatch};
    }
  } else if (!ExecutionDataPacketUuidIsNil(descriptor.map_key_descriptor_uuid)) {
    return {ContainerRepresentationStatus::map_key_descriptor_not_allowed};
  }

  if (field_based) {
    if (descriptor.storage_kind != ContainerStorageKind::row_tuple) {
      return {ContainerRepresentationStatus::storage_kind_incompatible};
    }
    if (descriptor.fields.empty()) {
      return {ContainerRepresentationStatus::fields_required};
    }
    if (descriptor.fields.size() > kContainerRepresentationMaxFields) {
      return {ContainerRepresentationStatus::field_count_exceeds_limit};
    }
    std::vector<Uuid> field_uuids;
    std::vector<std::string> field_names;
    field_uuids.reserve(descriptor.fields.size());
    field_names.reserve(descriptor.fields.size());
    for (std::size_t index = 0; index < descriptor.fields.size(); ++index) {
      const auto& field = descriptor.fields[index];
      if (ExecutionDataPacketUuidIsNil(field.field_uuid)) {
        return {ContainerRepresentationStatus::field_uuid_required,
                ExecutionDataPacketStatus::ok, index};
      }
      if (field.stable_name.empty()) {
        return {ContainerRepresentationStatus::field_stable_name_required,
                ExecutionDataPacketStatus::ok, index};
      }
      if (field.ordinal != index) {
        return {ContainerRepresentationStatus::field_ordinal_mismatch,
                ExecutionDataPacketStatus::ok, index};
      }
      if (ExecutionDataPacketUuidIsNil(field.element_descriptor_uuid)) {
        return {ContainerRepresentationStatus::field_descriptor_uuid_required,
                ExecutionDataPacketStatus::ok, index};
      }
      for (const auto& seen_uuid : field_uuids) {
        if (ExecutionDataPacketUuidEquals(seen_uuid, field.field_uuid)) {
          return {ContainerRepresentationStatus::field_uuid_duplicate,
                  ExecutionDataPacketStatus::ok, index};
        }
      }
      field_uuids.push_back(field.field_uuid);
      const auto field_name_token =
          ExecutionTypeModifierCanonicalToken(field.stable_name);
      for (const auto& seen_name : field_names) {
        if (seen_name == field_name_token) {
          return {ContainerRepresentationStatus::field_name_duplicate,
                  ExecutionDataPacketStatus::ok, index};
        }
      }
      field_names.push_back(field_name_token);

      const auto field_result =
          ValidateExecutionDataPacketDescriptor(field.element_descriptor, index);
      if (!field_result.ok()) {
        return {ContainerRepresentationStatus::field_descriptor_invalid,
                field_result.status, index};
      }
      if (!ExecutionDataPacketUuidEquals(
              field.element_descriptor.descriptor_uuid,
              field.element_descriptor_uuid)) {
        return {ContainerRepresentationStatus::field_descriptor_uuid_mismatch,
                ExecutionDataPacketStatus::ok, index};
      }
    }
  } else if (!descriptor.fields.empty()) {
    return {ContainerRepresentationStatus::fields_not_allowed};
  }

  return {};
}

// SEARCH_KEY: variant tag
// SEARCH_KEY: TAGGED_UNION
// SEARCH_KEY: active member
inline constexpr std::size_t kVariantRepresentationMaxMembers = 1024;

enum class VariantRepresentationKind : std::uint8_t {
  tagged_union = 0
};

constexpr bool VariantRepresentationKindIsValid(
    VariantRepresentationKind kind) noexcept {
  switch (kind) {
    case VariantRepresentationKind::tagged_union:
      return true;
  }
  return false;
}

struct VariantMemberDescriptor {
  Uuid member_uuid{};
  Uuid tag_uuid{};
  std::uint32_t ordinal = 0;
  std::string tag_name;
  Uuid value_descriptor_uuid{};
  ExecutionTypeDescriptor value_descriptor;
};

struct VariantRepresentationDescriptor {
  Uuid variant_uuid{};
  Uuid representation_descriptor_uuid{};
  Uuid tag_descriptor_uuid{};
  std::uint64_t descriptor_epoch = 0;
  std::string stable_name;
  VariantRepresentationKind representation_kind =
      VariantRepresentationKind::tagged_union;
  ExecutionTypeDescriptor tag_descriptor;
  std::vector<VariantMemberDescriptor> members;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct VariantActiveMemberValue {
  Uuid member_uuid{};
  Uuid tag_uuid{};
  std::uint32_t ordinal = 0;
  Uuid value_descriptor_uuid{};
  ExecutionValueState value_state = ExecutionValueState::value;
  std::vector<std::uint8_t> payload;
};

struct VariantActiveValueDescriptor {
  Uuid variant_uuid{};
  Uuid representation_descriptor_uuid{};
  std::vector<VariantActiveMemberValue> active_members;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

enum class VariantRepresentationStatus : std::uint8_t {
  ok = 0,
  variant_uuid_required = 1,
  representation_descriptor_uuid_required = 2,
  tag_descriptor_uuid_required = 3,
  descriptor_epoch_required = 4,
  stable_name_required = 5,
  descriptor_not_authoritative = 6,
  descriptor_parser_dependent = 7,
  representation_kind_invalid = 8,
  tag_descriptor_invalid = 9,
  tag_descriptor_uuid_mismatch = 10,
  tag_descriptor_nullable_not_allowed = 11,
  members_required = 12,
  member_count_exceeds_limit = 13,
  member_uuid_required = 14,
  member_tag_uuid_required = 15,
  member_tag_name_required = 16,
  member_ordinal_mismatch = 17,
  member_descriptor_uuid_required = 18,
  member_descriptor_invalid = 19,
  member_descriptor_uuid_mismatch = 20,
  member_uuid_duplicate = 21,
  member_tag_uuid_duplicate = 22,
  member_tag_name_duplicate = 23,
  active_variant_uuid_mismatch = 24,
  active_representation_descriptor_uuid_mismatch = 25,
  active_value_count_invalid = 26,
  active_member_uuid_required = 27,
  active_tag_uuid_required = 28,
  active_member_not_declared = 29,
  active_tag_mismatch = 30,
  active_ordinal_mismatch = 31,
  active_descriptor_uuid_mismatch = 32,
  active_value_state_invalid = 33,
  active_value_null_not_allowed = 34,
  active_payload_required = 35,
  active_payload_not_allowed = 36
};

constexpr std::string_view VariantRepresentationStatusName(
    VariantRepresentationStatus status) noexcept {
  switch (status) {
    case VariantRepresentationStatus::ok:
      return "ok";
    case VariantRepresentationStatus::variant_uuid_required:
      return "variant_uuid_required";
    case VariantRepresentationStatus::representation_descriptor_uuid_required:
      return "representation_descriptor_uuid_required";
    case VariantRepresentationStatus::tag_descriptor_uuid_required:
      return "tag_descriptor_uuid_required";
    case VariantRepresentationStatus::descriptor_epoch_required:
      return "descriptor_epoch_required";
    case VariantRepresentationStatus::stable_name_required:
      return "stable_name_required";
    case VariantRepresentationStatus::descriptor_not_authoritative:
      return "descriptor_not_authoritative";
    case VariantRepresentationStatus::descriptor_parser_dependent:
      return "descriptor_parser_dependent";
    case VariantRepresentationStatus::representation_kind_invalid:
      return "representation_kind_invalid";
    case VariantRepresentationStatus::tag_descriptor_invalid:
      return "tag_descriptor_invalid";
    case VariantRepresentationStatus::tag_descriptor_uuid_mismatch:
      return "tag_descriptor_uuid_mismatch";
    case VariantRepresentationStatus::tag_descriptor_nullable_not_allowed:
      return "tag_descriptor_nullable_not_allowed";
    case VariantRepresentationStatus::members_required:
      return "members_required";
    case VariantRepresentationStatus::member_count_exceeds_limit:
      return "member_count_exceeds_limit";
    case VariantRepresentationStatus::member_uuid_required:
      return "member_uuid_required";
    case VariantRepresentationStatus::member_tag_uuid_required:
      return "member_tag_uuid_required";
    case VariantRepresentationStatus::member_tag_name_required:
      return "member_tag_name_required";
    case VariantRepresentationStatus::member_ordinal_mismatch:
      return "member_ordinal_mismatch";
    case VariantRepresentationStatus::member_descriptor_uuid_required:
      return "member_descriptor_uuid_required";
    case VariantRepresentationStatus::member_descriptor_invalid:
      return "member_descriptor_invalid";
    case VariantRepresentationStatus::member_descriptor_uuid_mismatch:
      return "member_descriptor_uuid_mismatch";
    case VariantRepresentationStatus::member_uuid_duplicate:
      return "member_uuid_duplicate";
    case VariantRepresentationStatus::member_tag_uuid_duplicate:
      return "member_tag_uuid_duplicate";
    case VariantRepresentationStatus::member_tag_name_duplicate:
      return "member_tag_name_duplicate";
    case VariantRepresentationStatus::active_variant_uuid_mismatch:
      return "active_variant_uuid_mismatch";
    case VariantRepresentationStatus::
        active_representation_descriptor_uuid_mismatch:
      return "active_representation_descriptor_uuid_mismatch";
    case VariantRepresentationStatus::active_value_count_invalid:
      return "active_value_count_invalid";
    case VariantRepresentationStatus::active_member_uuid_required:
      return "active_member_uuid_required";
    case VariantRepresentationStatus::active_tag_uuid_required:
      return "active_tag_uuid_required";
    case VariantRepresentationStatus::active_member_not_declared:
      return "active_member_not_declared";
    case VariantRepresentationStatus::active_tag_mismatch:
      return "active_tag_mismatch";
    case VariantRepresentationStatus::active_ordinal_mismatch:
      return "active_ordinal_mismatch";
    case VariantRepresentationStatus::active_descriptor_uuid_mismatch:
      return "active_descriptor_uuid_mismatch";
    case VariantRepresentationStatus::active_value_state_invalid:
      return "active_value_state_invalid";
    case VariantRepresentationStatus::active_value_null_not_allowed:
      return "active_value_null_not_allowed";
    case VariantRepresentationStatus::active_payload_required:
      return "active_payload_required";
    case VariantRepresentationStatus::active_payload_not_allowed:
      return "active_payload_not_allowed";
  }
  return "unknown_status";
}

struct VariantRepresentationValidationResult {
  VariantRepresentationStatus status = VariantRepresentationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  std::size_t member_index = 0;

  bool ok() const noexcept {
    return status == VariantRepresentationStatus::ok;
  }
};

inline VariantRepresentationValidationResult
ValidateVariantRepresentationDescriptor(
    const VariantRepresentationDescriptor& descriptor) {
  if (ExecutionDataPacketUuidIsNil(descriptor.variant_uuid)) {
    return {VariantRepresentationStatus::variant_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(
          descriptor.representation_descriptor_uuid)) {
    return {
        VariantRepresentationStatus::representation_descriptor_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(descriptor.tag_descriptor_uuid)) {
    return {VariantRepresentationStatus::tag_descriptor_uuid_required};
  }
  if (descriptor.descriptor_epoch == 0) {
    return {VariantRepresentationStatus::descriptor_epoch_required};
  }
  if (descriptor.stable_name.empty()) {
    return {VariantRepresentationStatus::stable_name_required};
  }
  if (!descriptor.descriptor_authoritative) {
    return {VariantRepresentationStatus::descriptor_not_authoritative};
  }
  if (!descriptor.parser_independent) {
    return {VariantRepresentationStatus::descriptor_parser_dependent};
  }
  if (!VariantRepresentationKindIsValid(descriptor.representation_kind)) {
    return {VariantRepresentationStatus::representation_kind_invalid};
  }
  const auto tag_result =
      ValidateExecutionDataPacketDescriptor(descriptor.tag_descriptor, 0);
  if (!tag_result.ok()) {
    return {VariantRepresentationStatus::tag_descriptor_invalid,
            tag_result.status};
  }
  if (!ExecutionDataPacketUuidEquals(
          descriptor.tag_descriptor.descriptor_uuid,
          descriptor.tag_descriptor_uuid)) {
    return {VariantRepresentationStatus::tag_descriptor_uuid_mismatch};
  }
  if (descriptor.tag_descriptor.nullable_allowed) {
    return {VariantRepresentationStatus::tag_descriptor_nullable_not_allowed};
  }
  if (descriptor.members.empty()) {
    return {VariantRepresentationStatus::members_required};
  }
  if (descriptor.members.size() > kVariantRepresentationMaxMembers) {
    return {VariantRepresentationStatus::member_count_exceeds_limit};
  }

  std::vector<Uuid> member_uuids;
  std::vector<Uuid> tag_uuids;
  std::vector<std::string> tag_names;
  member_uuids.reserve(descriptor.members.size());
  tag_uuids.reserve(descriptor.members.size());
  tag_names.reserve(descriptor.members.size());
  for (std::size_t index = 0; index < descriptor.members.size(); ++index) {
    const auto& member = descriptor.members[index];
    if (ExecutionDataPacketUuidIsNil(member.member_uuid)) {
      return {VariantRepresentationStatus::member_uuid_required,
              ExecutionDataPacketStatus::ok, index};
    }
    if (ExecutionDataPacketUuidIsNil(member.tag_uuid)) {
      return {VariantRepresentationStatus::member_tag_uuid_required,
              ExecutionDataPacketStatus::ok, index};
    }
    if (member.tag_name.empty()) {
      return {VariantRepresentationStatus::member_tag_name_required,
              ExecutionDataPacketStatus::ok, index};
    }
    if (member.ordinal != index) {
      return {VariantRepresentationStatus::member_ordinal_mismatch,
              ExecutionDataPacketStatus::ok, index};
    }
    if (ExecutionDataPacketUuidIsNil(member.value_descriptor_uuid)) {
      return {VariantRepresentationStatus::member_descriptor_uuid_required,
              ExecutionDataPacketStatus::ok, index};
    }
    for (const auto& seen_uuid : member_uuids) {
      if (ExecutionDataPacketUuidEquals(seen_uuid, member.member_uuid)) {
        return {VariantRepresentationStatus::member_uuid_duplicate,
                ExecutionDataPacketStatus::ok, index};
      }
    }
    member_uuids.push_back(member.member_uuid);
    for (const auto& seen_tag_uuid : tag_uuids) {
      if (ExecutionDataPacketUuidEquals(seen_tag_uuid, member.tag_uuid)) {
        return {VariantRepresentationStatus::member_tag_uuid_duplicate,
                ExecutionDataPacketStatus::ok, index};
      }
    }
    tag_uuids.push_back(member.tag_uuid);
    const auto tag_name_token =
        ExecutionTypeModifierCanonicalToken(member.tag_name);
    for (const auto& seen_name : tag_names) {
      if (seen_name == tag_name_token) {
        return {VariantRepresentationStatus::member_tag_name_duplicate,
                ExecutionDataPacketStatus::ok, index};
      }
    }
    tag_names.push_back(tag_name_token);

    const auto member_result =
        ValidateExecutionDataPacketDescriptor(member.value_descriptor, index);
    if (!member_result.ok()) {
      return {VariantRepresentationStatus::member_descriptor_invalid,
              member_result.status, index};
    }
    if (!ExecutionDataPacketUuidEquals(
            member.value_descriptor.descriptor_uuid,
            member.value_descriptor_uuid)) {
      return {VariantRepresentationStatus::member_descriptor_uuid_mismatch,
              ExecutionDataPacketStatus::ok, index};
    }
  }
  return {};
}

inline VariantRepresentationValidationResult
ValidateVariantActiveValue(
    const VariantRepresentationDescriptor& representation,
    const VariantActiveValueDescriptor& active_value) {
  const auto representation_result =
      ValidateVariantRepresentationDescriptor(representation);
  if (!representation_result.ok()) {
    return representation_result;
  }
  if (!active_value.descriptor_authoritative) {
    return {VariantRepresentationStatus::descriptor_not_authoritative};
  }
  if (!active_value.parser_independent) {
    return {VariantRepresentationStatus::descriptor_parser_dependent};
  }
  if (!ExecutionDataPacketUuidEquals(active_value.variant_uuid,
                                     representation.variant_uuid)) {
    return {VariantRepresentationStatus::active_variant_uuid_mismatch};
  }
  if (!ExecutionDataPacketUuidEquals(
          active_value.representation_descriptor_uuid,
          representation.representation_descriptor_uuid)) {
    return {VariantRepresentationStatus::
                active_representation_descriptor_uuid_mismatch};
  }
  if (active_value.active_members.size() != 1) {
    return {VariantRepresentationStatus::active_value_count_invalid};
  }

  const auto& active_member = active_value.active_members.front();
  if (ExecutionDataPacketUuidIsNil(active_member.member_uuid)) {
    return {VariantRepresentationStatus::active_member_uuid_required};
  }
  if (ExecutionDataPacketUuidIsNil(active_member.tag_uuid)) {
    return {VariantRepresentationStatus::active_tag_uuid_required};
  }

  const VariantMemberDescriptor* declared_member = nullptr;
  std::size_t declared_index = 0;
  for (std::size_t index = 0; index < representation.members.size(); ++index) {
    if (ExecutionDataPacketUuidEquals(representation.members[index].member_uuid,
                                      active_member.member_uuid)) {
      declared_member = &representation.members[index];
      declared_index = index;
      break;
    }
  }
  if (declared_member == nullptr) {
    return {VariantRepresentationStatus::active_member_not_declared};
  }
  if (!ExecutionDataPacketUuidEquals(active_member.tag_uuid,
                                     declared_member->tag_uuid)) {
    return {VariantRepresentationStatus::active_tag_mismatch,
            ExecutionDataPacketStatus::ok, declared_index};
  }
  if (active_member.ordinal != declared_member->ordinal) {
    return {VariantRepresentationStatus::active_ordinal_mismatch,
            ExecutionDataPacketStatus::ok, declared_index};
  }
  if (!ExecutionDataPacketUuidEquals(active_member.value_descriptor_uuid,
                                     declared_member->value_descriptor_uuid)) {
    return {VariantRepresentationStatus::active_descriptor_uuid_mismatch,
            ExecutionDataPacketStatus::ok, declared_index};
  }

  switch (active_member.value_state) {
    case ExecutionValueState::value:
    case ExecutionValueState::lob_handle:
    case ExecutionValueState::protected_value:
      if (active_member.payload.empty()) {
        return {VariantRepresentationStatus::active_payload_required,
                ExecutionDataPacketStatus::ok, declared_index};
      }
      break;
    case ExecutionValueState::sql_null:
      if (!declared_member->value_descriptor.nullable_allowed) {
        return {VariantRepresentationStatus::active_value_null_not_allowed,
                ExecutionDataPacketStatus::ok, declared_index};
      }
      if (!active_member.payload.empty()) {
        return {VariantRepresentationStatus::active_payload_not_allowed,
                ExecutionDataPacketStatus::ok, declared_index};
      }
      break;
    case ExecutionValueState::missing:
    case ExecutionValueState::default_requested:
    case ExecutionValueState::unknown:
    case ExecutionValueState::error:
      return {VariantRepresentationStatus::active_value_state_invalid,
              ExecutionDataPacketStatus::ok, declared_index};
  }
  return {};
}

struct ExecutionResultEnvelopeMetadata {
  std::uint8_t major_version = kExecutionResultEnvelopeMajorVersion;
  std::uint8_t minor_version = kExecutionResultEnvelopeMinorVersion;
  ExecutionResultEnvelopeKind result_kind = ExecutionResultEnvelopeKind::row_set;
  std::string result_shape_id;
  std::string statement_uuid;
  bool descriptor_authoritative = true;
  bool parser_independent = true;
};

struct ExecutionResultCompletion {
  bool present = false;
  bool success = false;
  bool final = true;
  std::string completion_code;
  std::uint64_t rows_affected = 0;
};

struct ExecutionResultSummary {
  std::uint64_t rows_produced = 0;
  std::uint64_t batch_count = 0;
  bool final_batch_seen = false;
};

struct ExecutionResultDiagnostic {
  bool error = false;
  std::string diagnostic_code;
  std::string safe_message_key;
  std::string detail;
};

struct ExecutionResultEnvelope {
  ExecutionResultEnvelopeMetadata metadata;
  std::vector<ResultColumnDescriptor> columns;
  std::vector<ExecutionRowBatch> row_batches;
  ExecutionResultCompletion completion;
  ExecutionResultSummary summary;
  std::vector<ExecutionResultDiagnostic> diagnostics;
};

enum class ExecutionResultEnvelopeStatus : std::uint8_t {
  ok = 0,
  unsupported_version = 1,
  metadata_required = 2,
  metadata_not_authoritative = 3,
  metadata_parser_dependent = 4,
  columns_required_for_rows = 5,
  column_ordinal_mismatch = 6,
  column_descriptor_invalid = 7,
  column_rendering_metadata_required = 8,
  row_batch_invalid = 9,
  row_batch_sequence_mismatch = 10,
  row_batch_final_not_last = 11,
  row_batch_shape_mismatch = 12,
  row_batch_column_descriptor_mismatch = 13,
  row_batch_column_nullability_mismatch = 14,
  completion_required = 15,
  completion_code_required = 16,
  completion_error_without_diagnostic = 17,
  summary_row_count_mismatch = 18,
  summary_batch_count_mismatch = 19,
  summary_final_batch_mismatch = 20,
  diagnostic_code_required = 21
};

constexpr std::string_view ExecutionResultEnvelopeStatusName(
    ExecutionResultEnvelopeStatus status) noexcept {
  switch (status) {
    case ExecutionResultEnvelopeStatus::ok:
      return "ok";
    case ExecutionResultEnvelopeStatus::unsupported_version:
      return "unsupported_version";
    case ExecutionResultEnvelopeStatus::metadata_required:
      return "metadata_required";
    case ExecutionResultEnvelopeStatus::metadata_not_authoritative:
      return "metadata_not_authoritative";
    case ExecutionResultEnvelopeStatus::metadata_parser_dependent:
      return "metadata_parser_dependent";
    case ExecutionResultEnvelopeStatus::columns_required_for_rows:
      return "columns_required_for_rows";
    case ExecutionResultEnvelopeStatus::column_ordinal_mismatch:
      return "column_ordinal_mismatch";
    case ExecutionResultEnvelopeStatus::column_descriptor_invalid:
      return "column_descriptor_invalid";
    case ExecutionResultEnvelopeStatus::column_rendering_metadata_required:
      return "column_rendering_metadata_required";
    case ExecutionResultEnvelopeStatus::row_batch_invalid:
      return "row_batch_invalid";
    case ExecutionResultEnvelopeStatus::row_batch_sequence_mismatch:
      return "row_batch_sequence_mismatch";
    case ExecutionResultEnvelopeStatus::row_batch_final_not_last:
      return "row_batch_final_not_last";
    case ExecutionResultEnvelopeStatus::row_batch_shape_mismatch:
      return "row_batch_shape_mismatch";
    case ExecutionResultEnvelopeStatus::row_batch_column_descriptor_mismatch:
      return "row_batch_column_descriptor_mismatch";
    case ExecutionResultEnvelopeStatus::row_batch_column_nullability_mismatch:
      return "row_batch_column_nullability_mismatch";
    case ExecutionResultEnvelopeStatus::completion_required:
      return "completion_required";
    case ExecutionResultEnvelopeStatus::completion_code_required:
      return "completion_code_required";
    case ExecutionResultEnvelopeStatus::completion_error_without_diagnostic:
      return "completion_error_without_diagnostic";
    case ExecutionResultEnvelopeStatus::summary_row_count_mismatch:
      return "summary_row_count_mismatch";
    case ExecutionResultEnvelopeStatus::summary_batch_count_mismatch:
      return "summary_batch_count_mismatch";
    case ExecutionResultEnvelopeStatus::summary_final_batch_mismatch:
      return "summary_final_batch_mismatch";
    case ExecutionResultEnvelopeStatus::diagnostic_code_required:
      return "diagnostic_code_required";
  }
  return "unknown_status";
}

struct ExecutionResultEnvelopeValidationResult {
  ExecutionResultEnvelopeStatus status = ExecutionResultEnvelopeStatus::ok;
  ExecutionDataPacketStatus packet_status = ExecutionDataPacketStatus::ok;
  ExecutionRowBatchStatus row_batch_status = ExecutionRowBatchStatus::ok;
  std::size_t batch_index = 0;
  std::size_t row_index = 0;
  std::size_t column_index = 0;

  bool ok() const noexcept {
    return status == ExecutionResultEnvelopeStatus::ok;
  }
};

inline bool ExecutionTypeDescriptorIdentityEquals(
    const ExecutionTypeDescriptor& left,
    const ExecutionTypeDescriptor& right) noexcept {
  if (left.descriptor_epoch != right.descriptor_epoch) {
    return false;
  }
  for (std::size_t index = 0; index < 16; ++index) {
    if (left.descriptor_uuid.bytes[index] != right.descriptor_uuid.bytes[index]) {
      return false;
    }
  }
  return true;
}

inline bool ExecutionResultEnvelopeHasErrorDiagnostic(
    const ExecutionResultEnvelope& envelope) {
  for (const auto& diagnostic : envelope.diagnostics) {
    if (diagnostic.error) {
      return true;
    }
  }
  return false;
}

inline ExecutionResultEnvelopeValidationResult ValidateExecutionResultEnvelope(
    const ExecutionResultEnvelope& envelope) {
  if (envelope.metadata.major_version != kExecutionResultEnvelopeMajorVersion ||
      envelope.metadata.minor_version != kExecutionResultEnvelopeMinorVersion) {
    return {ExecutionResultEnvelopeStatus::unsupported_version};
  }
  if (envelope.metadata.result_shape_id.empty()) {
    return {ExecutionResultEnvelopeStatus::metadata_required};
  }
  if (!envelope.metadata.descriptor_authoritative) {
    return {ExecutionResultEnvelopeStatus::metadata_not_authoritative};
  }
  if (!envelope.metadata.parser_independent) {
    return {ExecutionResultEnvelopeStatus::metadata_parser_dependent};
  }
  if (!envelope.row_batches.empty() && envelope.columns.empty()) {
    return {ExecutionResultEnvelopeStatus::columns_required_for_rows};
  }

  for (std::size_t column_index = 0; column_index < envelope.columns.size();
       ++column_index) {
    const auto& column = envelope.columns[column_index];
    if (column.ordinal != column_index) {
      return {ExecutionResultEnvelopeStatus::column_ordinal_mismatch,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok, 0, 0,
              column_index};
    }
    const auto descriptor_result =
        ValidateExecutionDataPacketDescriptor(column.descriptor, column_index);
    if (!descriptor_result.ok()) {
      return {ExecutionResultEnvelopeStatus::column_descriptor_invalid,
              descriptor_result.status, ExecutionRowBatchStatus::ok, 0, 0,
              column_index};
    }
    if (column.semantic_name.empty() || column.native_rendering_name.empty() ||
        column.reference_rendering_name.empty()) {
      return {ExecutionResultEnvelopeStatus::column_rendering_metadata_required,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok, 0, 0,
              column_index};
    }
  }

  std::uint64_t observed_rows = 0;
  bool final_batch_seen = false;
  for (std::size_t batch_index = 0; batch_index < envelope.row_batches.size();
       ++batch_index) {
    const auto& batch = envelope.row_batches[batch_index];
    const auto batch_result = ValidateExecutionRowBatch(batch);
    if (!batch_result.ok()) {
      return {ExecutionResultEnvelopeStatus::row_batch_invalid,
              batch_result.packet_status, batch_result.status, batch_index,
              batch_result.row_index, batch_result.column_index};
    }
    if (batch.batch_sequence != batch_index) {
      return {ExecutionResultEnvelopeStatus::row_batch_sequence_mismatch,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok,
              batch_index, 0, 0};
    }
    if (batch.final_batch && batch_index + 1 != envelope.row_batches.size()) {
      return {ExecutionResultEnvelopeStatus::row_batch_final_not_last,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok,
              batch_index, 0, 0};
    }
    final_batch_seen = final_batch_seen || batch.final_batch;
    if (batch.row_shape.columns.size() != envelope.columns.size()) {
      return {ExecutionResultEnvelopeStatus::row_batch_shape_mismatch,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok,
              batch_index, 0, 0};
    }
    for (std::size_t column_index = 0;
         column_index < batch.row_shape.columns.size(); ++column_index) {
      const auto descriptor_index =
          batch.row_shape.columns[column_index].descriptor_index;
      const auto& batch_descriptor =
          batch.data_packet.descriptor_table[descriptor_index];
      if (!ExecutionTypeDescriptorIdentityEquals(
              envelope.columns[column_index].descriptor, batch_descriptor)) {
        return {
            ExecutionResultEnvelopeStatus::row_batch_column_descriptor_mismatch,
            ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok,
            batch_index, 0, column_index};
      }
      if (batch.row_shape.columns[column_index].nullable !=
          envelope.columns[column_index].nullable) {
        return {
            ExecutionResultEnvelopeStatus::row_batch_column_nullability_mismatch,
            ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok,
            batch_index, 0, column_index};
      }
    }
    observed_rows += batch.rows.size();
  }

  if (!envelope.completion.present) {
    return {ExecutionResultEnvelopeStatus::completion_required};
  }
  if (envelope.completion.completion_code.empty()) {
    return {ExecutionResultEnvelopeStatus::completion_code_required};
  }
  if (!envelope.completion.success &&
      !ExecutionResultEnvelopeHasErrorDiagnostic(envelope)) {
    return {ExecutionResultEnvelopeStatus::completion_error_without_diagnostic};
  }
  if (envelope.summary.rows_produced != observed_rows) {
    return {ExecutionResultEnvelopeStatus::summary_row_count_mismatch};
  }
  if (envelope.summary.batch_count != envelope.row_batches.size()) {
    return {ExecutionResultEnvelopeStatus::summary_batch_count_mismatch};
  }
  if (envelope.summary.final_batch_seen != final_batch_seen) {
    return {ExecutionResultEnvelopeStatus::summary_final_batch_mismatch};
  }
  for (std::size_t diagnostic_index = 0;
       diagnostic_index < envelope.diagnostics.size(); ++diagnostic_index) {
    if (envelope.diagnostics[diagnostic_index].diagnostic_code.empty() ||
        envelope.diagnostics[diagnostic_index].safe_message_key.empty()) {
      return {ExecutionResultEnvelopeStatus::diagnostic_code_required,
              ExecutionDataPacketStatus::ok, ExecutionRowBatchStatus::ok, 0,
              diagnostic_index, 0};
    }
  }
  return {};
}

// SEARCH_KEY: EDR_EXECUTION_COERCION_CONTEXT
enum class ExecutionCoercionCategory : std::uint8_t {
  identity = 0,
  lossless_implicit = 1,
  lossless_explicit = 2,
  lossy_explicit = 3,
  reference_compatibility_explicit = 4,
  domain_to_base = 5,
  base_to_domain = 6,
  forbidden = 7
};

enum class ExecutionCoercionFailureIdentity : std::uint8_t {
  none = 0,
  source_descriptor_invalid = 1,
  target_descriptor_invalid = 2,
  source_value_state_invalid = 3,
  source_value_state_not_coercible = 4,
  target_nullability_violation = 5,
  explicit_cast_required = 6,
  lossy_cast_not_allowed = 7,
  reference_profile_required = 8,
  domain_boundary_not_allowed = 9,
  domain_metadata_required = 10,
  descriptor_identity_mismatch = 11,
  coercion_forbidden = 12,
  invalid_text_representation = 13
};

constexpr std::string_view ExecutionCoercionFailureIdentityName(
    ExecutionCoercionFailureIdentity identity) noexcept {
  switch (identity) {
    case ExecutionCoercionFailureIdentity::none:
      return "OK";
    case ExecutionCoercionFailureIdentity::source_descriptor_invalid:
      return "SOURCE_DESCRIPTOR_INVALID";
    case ExecutionCoercionFailureIdentity::target_descriptor_invalid:
      return "TARGET_DESCRIPTOR_INVALID";
    case ExecutionCoercionFailureIdentity::source_value_state_invalid:
      return "SOURCE_VALUE_STATE_INVALID";
    case ExecutionCoercionFailureIdentity::source_value_state_not_coercible:
      return "SOURCE_VALUE_STATE_NOT_COERCIBLE";
    case ExecutionCoercionFailureIdentity::target_nullability_violation:
      return "TARGET_NULLABILITY_VIOLATION";
    case ExecutionCoercionFailureIdentity::explicit_cast_required:
      return "EXPLICIT_CAST_REQUIRED";
    case ExecutionCoercionFailureIdentity::lossy_cast_not_allowed:
      return "LOSSY_CAST_NOT_ALLOWED";
    case ExecutionCoercionFailureIdentity::reference_profile_required:
      return "REFERENCE_PROFILE_REQUIRED";
    case ExecutionCoercionFailureIdentity::domain_boundary_not_allowed:
      return "DOMAIN_BOUNDARY_NOT_ALLOWED";
    case ExecutionCoercionFailureIdentity::domain_metadata_required:
      return "DOMAIN_METADATA_REQUIRED";
    case ExecutionCoercionFailureIdentity::descriptor_identity_mismatch:
      return "DESCRIPTOR_IDENTITY_MISMATCH";
    case ExecutionCoercionFailureIdentity::coercion_forbidden:
      return "COERCION_FORBIDDEN";
    case ExecutionCoercionFailureIdentity::invalid_text_representation:
      return "INVALID_TEXT_REPRESENTATION";
  }
  return "UNKNOWN_COERCION_FAILURE";
}

struct ExecutionCoercionContext {
  bool explicit_cast = false;
  bool allow_lossy = false;
  bool reference_compatibility_profile = false;
  bool allow_domain_boundary = false;
  bool require_identity_descriptor_match = true;
  bool source_payload_text_valid = true;
};

struct ExecutionCoercionRequest {
  ExecutionTypeDescriptor source_descriptor;
  ExecutionTypeDescriptor target_descriptor;
  ExecutionValueState source_state = ExecutionValueState::value;
  ExecutionCoercionCategory category = ExecutionCoercionCategory::forbidden;
  ExecutionCoercionContext context;
};

struct ExecutionCoercionValidationResult {
  ExecutionCoercionFailureIdentity failure_identity =
      ExecutionCoercionFailureIdentity::none;
  ExecutionCoercionCategory accepted_category =
      ExecutionCoercionCategory::forbidden;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return failure_identity == ExecutionCoercionFailureIdentity::none;
  }

  std::string_view stableFailureIdentity() const noexcept {
    return ExecutionCoercionFailureIdentityName(failure_identity);
  }
};

inline ExecutionCoercionValidationResult ExecutionCoercionOk(
    ExecutionCoercionCategory category) {
  return {ExecutionCoercionFailureIdentity::none, category,
          ExecutionDataPacketStatus::ok};
}

inline ExecutionCoercionValidationResult ExecutionCoercionFailure(
    ExecutionCoercionFailureIdentity failure_identity,
    ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok) {
  return {failure_identity, ExecutionCoercionCategory::forbidden,
          descriptor_status};
}

inline bool ExecutionTypeFamilyIsNumeric(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::signed_integer ||
         family == ExecutionTypeFamily::unsigned_integer ||
         family == ExecutionTypeFamily::real ||
         family == ExecutionTypeFamily::decimal;
}

inline bool ExecutionTypeDescriptorCanWidenLosslessly(
    const ExecutionTypeDescriptor& source,
    const ExecutionTypeDescriptor& target) noexcept {
  if (ExecutionTypeDescriptorIdentityEquals(source, target)) {
    return true;
  }
  if (source.family != target.family) {
    return false;
  }
  if (source.family == ExecutionTypeFamily::decimal) {
    return target.precision >= source.precision &&
           target.scale >= source.scale;
  }
  if (source.family == ExecutionTypeFamily::character ||
      source.family == ExecutionTypeFamily::binary ||
      source.family == ExecutionTypeFamily::bit_string) {
    return source.length != 0 && target.length >= source.length;
  }
  if (ExecutionTypeFamilyIsNumeric(source.family) ||
      source.family == ExecutionTypeFamily::boolean ||
      source.family == ExecutionTypeFamily::uuid) {
    return source.bit_width != 0 && target.bit_width >= source.bit_width;
  }
  return false;
}

inline bool ExecutionTypeDescriptorsShareExplicitPath(
    const ExecutionTypeDescriptor& source,
    const ExecutionTypeDescriptor& target) noexcept {
  return source.family == target.family ||
         (ExecutionTypeFamilyIsNumeric(source.family) &&
          ExecutionTypeFamilyIsNumeric(target.family));
}

inline bool ExecutionTypeDescriptorHasDomainMetadata(
    const ExecutionTypeDescriptor& descriptor) noexcept {
  return ExecutionTypeDescriptorHasModifierFlag(
             descriptor, ExecutionTypeModifierFlag::domain_uuid) &&
         !ExecutionDataPacketUuidIsNil(descriptor.domain_uuid);
}

inline ExecutionCoercionValidationResult ValidateExecutionCoercionRequest(
    const ExecutionCoercionRequest& request) {
  const auto source_descriptor_result =
      ValidateExecutionDataPacketDescriptor(request.source_descriptor, 0);
  if (!source_descriptor_result.ok()) {
    return ExecutionCoercionFailure(
        ExecutionCoercionFailureIdentity::source_descriptor_invalid,
        source_descriptor_result.status);
  }

  const auto target_descriptor_result =
      ValidateExecutionDataPacketDescriptor(request.target_descriptor, 1);
  if (!target_descriptor_result.ok()) {
    return ExecutionCoercionFailure(
        ExecutionCoercionFailureIdentity::target_descriptor_invalid,
        target_descriptor_result.status);
  }

  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(request.source_state))) {
    return ExecutionCoercionFailure(
        ExecutionCoercionFailureIdentity::source_value_state_invalid);
  }

  if (request.source_state == ExecutionValueState::sql_null) {
    if (!request.target_descriptor.nullable_allowed) {
      return ExecutionCoercionFailure(
          ExecutionCoercionFailureIdentity::target_nullability_violation);
    }
    return ExecutionCoercionOk(request.category);
  }

  if (request.source_state != ExecutionValueState::value) {
    return ExecutionCoercionFailure(
        ExecutionCoercionFailureIdentity::source_value_state_not_coercible);
  }

  if (!request.context.source_payload_text_valid) {
    return ExecutionCoercionFailure(
        ExecutionCoercionFailureIdentity::invalid_text_representation);
  }

  switch (request.category) {
    case ExecutionCoercionCategory::identity:
      if (request.context.require_identity_descriptor_match &&
          !ExecutionTypeDescriptorIdentityEquals(request.source_descriptor,
                                                request.target_descriptor)) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::descriptor_identity_mismatch);
      }
      return ExecutionCoercionOk(request.category);

    case ExecutionCoercionCategory::lossless_implicit:
      if (ExecutionTypeDescriptorCanWidenLosslessly(request.source_descriptor,
                                                   request.target_descriptor)) {
        return ExecutionCoercionOk(request.category);
      }
      return ExecutionCoercionFailure(
          ExecutionCoercionFailureIdentity::coercion_forbidden);

    case ExecutionCoercionCategory::lossless_explicit:
      if (!request.context.explicit_cast) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::explicit_cast_required);
      }
      if (ExecutionTypeDescriptorCanWidenLosslessly(request.source_descriptor,
                                                   request.target_descriptor)) {
        return ExecutionCoercionOk(request.category);
      }
      return ExecutionCoercionFailure(
          ExecutionCoercionFailureIdentity::coercion_forbidden);

    case ExecutionCoercionCategory::lossy_explicit:
      if (!request.context.explicit_cast) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::explicit_cast_required);
      }
      if (!request.context.allow_lossy) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::lossy_cast_not_allowed);
      }
      if (ExecutionTypeDescriptorsShareExplicitPath(request.source_descriptor,
                                                   request.target_descriptor)) {
        return ExecutionCoercionOk(request.category);
      }
      return ExecutionCoercionFailure(
          ExecutionCoercionFailureIdentity::coercion_forbidden);

    case ExecutionCoercionCategory::reference_compatibility_explicit:
      if (!request.context.explicit_cast) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::explicit_cast_required);
      }
      if (!request.context.reference_compatibility_profile) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::reference_profile_required);
      }
      return ExecutionCoercionOk(request.category);

    case ExecutionCoercionCategory::domain_to_base:
      if (!request.context.allow_domain_boundary) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::domain_boundary_not_allowed);
      }
      if (!ExecutionTypeDescriptorHasDomainMetadata(
              request.source_descriptor) ||
          ExecutionTypeDescriptorHasDomainMetadata(request.target_descriptor)) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::domain_metadata_required);
      }
      return ExecutionCoercionOk(request.category);

    case ExecutionCoercionCategory::base_to_domain:
      if (!request.context.allow_domain_boundary) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::domain_boundary_not_allowed);
      }
      if (ExecutionTypeDescriptorHasDomainMetadata(request.source_descriptor) ||
          !ExecutionTypeDescriptorHasDomainMetadata(request.target_descriptor)) {
        return ExecutionCoercionFailure(
            ExecutionCoercionFailureIdentity::domain_metadata_required);
      }
      return ExecutionCoercionOk(request.category);

    case ExecutionCoercionCategory::forbidden:
      return ExecutionCoercionFailure(
          ExecutionCoercionFailureIdentity::coercion_forbidden);
  }

  return ExecutionCoercionFailure(
      ExecutionCoercionFailureIdentity::coercion_forbidden);
}

inline ExecutionCoercionValidationResult convertTo(
    const ExecutionCoercionRequest& request) {
  return ValidateExecutionCoercionRequest(request);
}

// SEARCH_KEY: EDR_TEXT_TEMPORAL_DESCRIPTOR_BOUND_SEMANTICS
enum class ExecutionTextTemporalOperation : std::uint8_t {
  text_render = 0,
  text_compare = 1,
  text_index_key = 2,
  temporal_render = 3,
  temporal_compare = 4,
  temporal_index_key = 5
};

enum class ExecutionTimezonePolicy : std::uint8_t {
  not_applicable = 0,
  descriptor_timezone_required = 1
};

struct ExecutionResourceBinding {
  Uuid resource_uuid{};
  std::uint64_t activation_epoch = 0;
  std::string version_token;
  bool available = true;
  bool parser_dependent = false;
};

struct ExecutionTextTemporalSemanticsRequest {
  ExecutionTypeDescriptor descriptor;
  ExecutionTextTemporalOperation operation =
      ExecutionTextTemporalOperation::text_render;
  ExecutionTimezonePolicy timezone_policy =
      ExecutionTimezonePolicy::not_applicable;
  ExecutionResourceBinding charset;
  ExecutionResourceBinding collation;
  ExecutionResourceBinding timezone;
};

enum class ExecutionTextTemporalSemanticsStatus : std::uint8_t {
  ok = 0,
  descriptor_invalid = 1,
  descriptor_family_unsupported = 2,
  charset_uuid_missing = 3,
  charset_binding_required = 4,
  charset_binding_mismatch = 5,
  charset_epoch_required = 6,
  charset_version_required = 7,
  charset_unavailable = 8,
  charset_parser_dependent = 9,
  collation_uuid_missing = 10,
  collation_binding_required = 11,
  collation_binding_mismatch = 12,
  collation_epoch_required = 13,
  collation_version_required = 14,
  collation_unavailable = 15,
  collation_parser_dependent = 16,
  timezone_policy_required = 17,
  timezone_uuid_missing = 18,
  timezone_binding_required = 19,
  timezone_binding_mismatch = 20,
  timezone_epoch_required = 21,
  timezone_version_required = 22,
  timezone_unavailable = 23,
  timezone_parser_dependent = 24
};

constexpr std::string_view ExecutionTextTemporalSemanticsStatusName(
    ExecutionTextTemporalSemanticsStatus status) noexcept {
  switch (status) {
    case ExecutionTextTemporalSemanticsStatus::ok:
      return "ok";
    case ExecutionTextTemporalSemanticsStatus::descriptor_invalid:
      return "descriptor_invalid";
    case ExecutionTextTemporalSemanticsStatus::descriptor_family_unsupported:
      return "descriptor_family_unsupported";
    case ExecutionTextTemporalSemanticsStatus::charset_uuid_missing:
      return "charset_uuid_missing";
    case ExecutionTextTemporalSemanticsStatus::charset_binding_required:
      return "charset_binding_required";
    case ExecutionTextTemporalSemanticsStatus::charset_binding_mismatch:
      return "charset_binding_mismatch";
    case ExecutionTextTemporalSemanticsStatus::charset_epoch_required:
      return "charset_epoch_required";
    case ExecutionTextTemporalSemanticsStatus::charset_version_required:
      return "charset_version_required";
    case ExecutionTextTemporalSemanticsStatus::charset_unavailable:
      return "charset_unavailable";
    case ExecutionTextTemporalSemanticsStatus::charset_parser_dependent:
      return "charset_parser_dependent";
    case ExecutionTextTemporalSemanticsStatus::collation_uuid_missing:
      return "collation_uuid_missing";
    case ExecutionTextTemporalSemanticsStatus::collation_binding_required:
      return "collation_binding_required";
    case ExecutionTextTemporalSemanticsStatus::collation_binding_mismatch:
      return "collation_binding_mismatch";
    case ExecutionTextTemporalSemanticsStatus::collation_epoch_required:
      return "collation_epoch_required";
    case ExecutionTextTemporalSemanticsStatus::collation_version_required:
      return "collation_version_required";
    case ExecutionTextTemporalSemanticsStatus::collation_unavailable:
      return "collation_unavailable";
    case ExecutionTextTemporalSemanticsStatus::collation_parser_dependent:
      return "collation_parser_dependent";
    case ExecutionTextTemporalSemanticsStatus::timezone_policy_required:
      return "timezone_policy_required";
    case ExecutionTextTemporalSemanticsStatus::timezone_uuid_missing:
      return "timezone_uuid_missing";
    case ExecutionTextTemporalSemanticsStatus::timezone_binding_required:
      return "timezone_binding_required";
    case ExecutionTextTemporalSemanticsStatus::timezone_binding_mismatch:
      return "timezone_binding_mismatch";
    case ExecutionTextTemporalSemanticsStatus::timezone_epoch_required:
      return "timezone_epoch_required";
    case ExecutionTextTemporalSemanticsStatus::timezone_version_required:
      return "timezone_version_required";
    case ExecutionTextTemporalSemanticsStatus::timezone_unavailable:
      return "timezone_unavailable";
    case ExecutionTextTemporalSemanticsStatus::timezone_parser_dependent:
      return "timezone_parser_dependent";
  }
  return "unknown_text_temporal_semantics_status";
}

struct ExecutionTextTemporalSemanticsValidationResult {
  ExecutionTextTemporalSemanticsStatus status =
      ExecutionTextTemporalSemanticsStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionTextTemporalSemanticsStatus::ok;
  }
};

inline bool ExecutionResourceUuidEquals(const Uuid& left,
                                        const Uuid& right) noexcept {
  for (std::size_t index = 0; index < 16; ++index) {
    if (left.bytes[index] != right.bytes[index]) {
      return false;
    }
  }
  return true;
}

inline ExecutionTextTemporalSemanticsValidationResult
ValidateExecutionResourceBinding(
    const Uuid& expected_uuid,
    const ExecutionResourceBinding& binding,
    ExecutionTextTemporalSemanticsStatus binding_required,
    ExecutionTextTemporalSemanticsStatus binding_mismatch,
    ExecutionTextTemporalSemanticsStatus epoch_required,
    ExecutionTextTemporalSemanticsStatus version_required,
    ExecutionTextTemporalSemanticsStatus unavailable,
    ExecutionTextTemporalSemanticsStatus parser_dependent) {
  if (ExecutionDataPacketUuidIsNil(binding.resource_uuid)) {
    return {binding_required};
  }
  if (!ExecutionResourceUuidEquals(expected_uuid, binding.resource_uuid)) {
    return {binding_mismatch};
  }
  if (binding.activation_epoch == 0) {
    return {epoch_required};
  }
  if (binding.version_token.empty()) {
    return {version_required};
  }
  if (!binding.available) {
    return {unavailable};
  }
  if (binding.parser_dependent) {
    return {parser_dependent};
  }
  return {};
}

inline ExecutionTextTemporalSemanticsValidationResult
ValidateExecutionCharsetBinding(
    const ExecutionTextTemporalSemanticsRequest& request) {
  if (!ExecutionTypeDescriptorHasModifierFlag(
          request.descriptor, ExecutionTypeModifierFlag::charset_uuid) ||
      ExecutionDataPacketUuidIsNil(request.descriptor.charset_uuid)) {
    return {ExecutionTextTemporalSemanticsStatus::charset_uuid_missing};
  }
  return ValidateExecutionResourceBinding(
      request.descriptor.charset_uuid, request.charset,
      ExecutionTextTemporalSemanticsStatus::charset_binding_required,
      ExecutionTextTemporalSemanticsStatus::charset_binding_mismatch,
      ExecutionTextTemporalSemanticsStatus::charset_epoch_required,
      ExecutionTextTemporalSemanticsStatus::charset_version_required,
      ExecutionTextTemporalSemanticsStatus::charset_unavailable,
      ExecutionTextTemporalSemanticsStatus::charset_parser_dependent);
}

inline ExecutionTextTemporalSemanticsValidationResult
ValidateExecutionCollationBinding(
    const ExecutionTextTemporalSemanticsRequest& request) {
  if (!ExecutionTypeDescriptorHasModifierFlag(
          request.descriptor, ExecutionTypeModifierFlag::collation_uuid) ||
      ExecutionDataPacketUuidIsNil(request.descriptor.collation_uuid)) {
    return {ExecutionTextTemporalSemanticsStatus::collation_uuid_missing};
  }
  return ValidateExecutionResourceBinding(
      request.descriptor.collation_uuid, request.collation,
      ExecutionTextTemporalSemanticsStatus::collation_binding_required,
      ExecutionTextTemporalSemanticsStatus::collation_binding_mismatch,
      ExecutionTextTemporalSemanticsStatus::collation_epoch_required,
      ExecutionTextTemporalSemanticsStatus::collation_version_required,
      ExecutionTextTemporalSemanticsStatus::collation_unavailable,
      ExecutionTextTemporalSemanticsStatus::collation_parser_dependent);
}

inline ExecutionTextTemporalSemanticsValidationResult
ValidateExecutionTimezoneBinding(
    const ExecutionTextTemporalSemanticsRequest& request) {
  if (!ExecutionTypeDescriptorHasModifierFlag(
          request.descriptor, ExecutionTypeModifierFlag::timezone_uuid) ||
      ExecutionDataPacketUuidIsNil(request.descriptor.timezone_uuid)) {
    return {ExecutionTextTemporalSemanticsStatus::timezone_uuid_missing};
  }
  return ValidateExecutionResourceBinding(
      request.descriptor.timezone_uuid, request.timezone,
      ExecutionTextTemporalSemanticsStatus::timezone_binding_required,
      ExecutionTextTemporalSemanticsStatus::timezone_binding_mismatch,
      ExecutionTextTemporalSemanticsStatus::timezone_epoch_required,
      ExecutionTextTemporalSemanticsStatus::timezone_version_required,
      ExecutionTextTemporalSemanticsStatus::timezone_unavailable,
      ExecutionTextTemporalSemanticsStatus::timezone_parser_dependent);
}

inline ExecutionTextTemporalSemanticsValidationResult
ValidateExecutionTextTemporalSemantics(
    const ExecutionTextTemporalSemanticsRequest& request) {
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(request.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionTextTemporalSemanticsStatus::descriptor_invalid,
            descriptor_result.status};
  }

  switch (request.operation) {
    case ExecutionTextTemporalOperation::text_render:
    case ExecutionTextTemporalOperation::text_compare:
    case ExecutionTextTemporalOperation::text_index_key: {
      if (request.descriptor.family != ExecutionTypeFamily::character) {
        return {
            ExecutionTextTemporalSemanticsStatus::descriptor_family_unsupported};
      }
      const auto charset_result = ValidateExecutionCharsetBinding(request);
      if (!charset_result.ok()) {
        return charset_result;
      }
      if (request.operation == ExecutionTextTemporalOperation::text_compare ||
          request.operation == ExecutionTextTemporalOperation::text_index_key) {
        return ValidateExecutionCollationBinding(request);
      }
      return {};
    }

    case ExecutionTextTemporalOperation::temporal_render:
    case ExecutionTextTemporalOperation::temporal_compare:
    case ExecutionTextTemporalOperation::temporal_index_key:
      if (request.descriptor.family != ExecutionTypeFamily::temporal) {
        return {
            ExecutionTextTemporalSemanticsStatus::descriptor_family_unsupported};
      }
      if (ExecutionTypeDescriptorHasModifierFlag(
              request.descriptor, ExecutionTypeModifierFlag::timezone_uuid) &&
          request.timezone_policy !=
              ExecutionTimezonePolicy::descriptor_timezone_required) {
        return {ExecutionTextTemporalSemanticsStatus::timezone_policy_required};
      }
      if (request.timezone_policy ==
          ExecutionTimezonePolicy::descriptor_timezone_required) {
        return ValidateExecutionTimezoneBinding(request);
      }
      return {};
  }

  return {ExecutionTextTemporalSemanticsStatus::descriptor_family_unsupported};
}

// SEARCH_KEY: EDR_LARGE_VALUE_HANDLE_LIFECYCLE
enum class ExecutionLargeValueKind : std::uint8_t {
  inline_payload = 0,
  external_value_handle = 1,
  lob_handle = 2,
  stream_handle = 3,
  chunked_value = 4
};

enum class ExecutionLargeValueLifecycleState : std::uint8_t {
  inline_value = 0,
  durable_uncommitted = 1,
  committed_visible = 2,
  rolled_back = 3,
  cleanup_pending = 4,
  cleanup_reclaimed = 5,
  quarantined = 6
};

struct ExecutionLargeValueHandle {
  ExecutionLargeValueKind kind = ExecutionLargeValueKind::lob_handle;
  ExecutionLargeValueLifecycleState lifecycle_state =
      ExecutionLargeValueLifecycleState::durable_uncommitted;
  ExecutionValueState value_state = ExecutionValueState::lob_handle;
  ExecutionTypeDescriptor descriptor;
  Uuid value_uuid{};
  Uuid owner_transaction_uuid{};
  Uuid visibility_snapshot_uuid{};
  Uuid cleanup_policy_uuid{};
  std::uint64_t owner_transaction_number = 0;
  std::uint64_t committed_transaction_number = 0;
  std::uint64_t total_bytes = 0;
  std::uint32_t chunk_count = 0;
  std::uint32_t chunk_payload_bytes = 0;
  std::string content_hash;
  bool integrity_verified = false;
  bool storage_reference_authoritative = true;
  bool parser_independent = true;
  bool stream_final = true;
};

struct ExecutionLargeValueVisibilityContext {
  Uuid reader_transaction_uuid{};
  std::uint64_t reader_transaction_number = 0;
  std::uint64_t visible_committed_high_watermark = 0;
  std::uint64_t authoritative_cleanup_horizon_transaction_number = 0;
  bool include_own_uncommitted = false;
  bool cleanup_horizon_authoritative = false;
};

enum class ExecutionLargeValueValidationStatus : std::uint8_t {
  ok = 0,
  descriptor_invalid = 1,
  descriptor_family_unsupported = 2,
  value_uuid_required = 3,
  value_state_invalid = 4,
  value_state_kind_mismatch = 5,
  owner_transaction_uuid_required = 6,
  owner_transaction_number_required = 7,
  total_bytes_required = 8,
  chunk_payload_bytes_required = 9,
  chunk_count_required = 10,
  chunk_count_mismatch = 11,
  content_hash_required = 12,
  integrity_required = 13,
  storage_reference_not_authoritative = 14,
  parser_dependent = 15,
  commit_inventory_required = 16,
  visibility_snapshot_required = 17,
  reader_transaction_required = 18,
  stream_final_required = 19,
  cleanup_horizon_not_authoritative = 20,
  cleanup_horizon_before_owner = 21,
  lifecycle_state_invalid = 22
};

constexpr std::string_view ExecutionLargeValueValidationStatusName(
    ExecutionLargeValueValidationStatus status) noexcept {
  switch (status) {
    case ExecutionLargeValueValidationStatus::ok:
      return "ok";
    case ExecutionLargeValueValidationStatus::descriptor_invalid:
      return "descriptor_invalid";
    case ExecutionLargeValueValidationStatus::descriptor_family_unsupported:
      return "descriptor_family_unsupported";
    case ExecutionLargeValueValidationStatus::value_uuid_required:
      return "value_uuid_required";
    case ExecutionLargeValueValidationStatus::value_state_invalid:
      return "value_state_invalid";
    case ExecutionLargeValueValidationStatus::value_state_kind_mismatch:
      return "value_state_kind_mismatch";
    case ExecutionLargeValueValidationStatus::owner_transaction_uuid_required:
      return "owner_transaction_uuid_required";
    case ExecutionLargeValueValidationStatus::owner_transaction_number_required:
      return "owner_transaction_number_required";
    case ExecutionLargeValueValidationStatus::total_bytes_required:
      return "total_bytes_required";
    case ExecutionLargeValueValidationStatus::chunk_payload_bytes_required:
      return "chunk_payload_bytes_required";
    case ExecutionLargeValueValidationStatus::chunk_count_required:
      return "chunk_count_required";
    case ExecutionLargeValueValidationStatus::chunk_count_mismatch:
      return "chunk_count_mismatch";
    case ExecutionLargeValueValidationStatus::content_hash_required:
      return "content_hash_required";
    case ExecutionLargeValueValidationStatus::integrity_required:
      return "integrity_required";
    case ExecutionLargeValueValidationStatus::storage_reference_not_authoritative:
      return "storage_reference_not_authoritative";
    case ExecutionLargeValueValidationStatus::parser_dependent:
      return "parser_dependent";
    case ExecutionLargeValueValidationStatus::commit_inventory_required:
      return "commit_inventory_required";
    case ExecutionLargeValueValidationStatus::visibility_snapshot_required:
      return "visibility_snapshot_required";
    case ExecutionLargeValueValidationStatus::reader_transaction_required:
      return "reader_transaction_required";
    case ExecutionLargeValueValidationStatus::stream_final_required:
      return "stream_final_required";
    case ExecutionLargeValueValidationStatus::cleanup_horizon_not_authoritative:
      return "cleanup_horizon_not_authoritative";
    case ExecutionLargeValueValidationStatus::cleanup_horizon_before_owner:
      return "cleanup_horizon_before_owner";
    case ExecutionLargeValueValidationStatus::lifecycle_state_invalid:
      return "lifecycle_state_invalid";
  }
  return "unknown_large_value_validation_status";
}

struct ExecutionLargeValueValidationResult {
  ExecutionLargeValueValidationStatus status =
      ExecutionLargeValueValidationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  bool visible = false;
  bool cleanup_eligible = false;

  bool ok() const noexcept {
    return status == ExecutionLargeValueValidationStatus::ok;
  }
};

inline bool ExecutionLargeValueDescriptorFamilySupported(
    ExecutionTypeFamily family) noexcept {
  return family == ExecutionTypeFamily::blob ||
         family == ExecutionTypeFamily::binary ||
         family == ExecutionTypeFamily::character ||
         family == ExecutionTypeFamily::document ||
         family == ExecutionTypeFamily::opaque;
}

inline bool ExecutionLargeValueUsesExternalStorage(
    ExecutionLargeValueKind kind) noexcept {
  return kind == ExecutionLargeValueKind::external_value_handle ||
         kind == ExecutionLargeValueKind::lob_handle ||
         kind == ExecutionLargeValueKind::stream_handle ||
         kind == ExecutionLargeValueKind::chunked_value;
}

inline std::uint64_t ExecutionLargeValueExpectedChunkCount(
    std::uint64_t total_bytes,
    std::uint32_t chunk_payload_bytes) noexcept {
  if (total_bytes == 0 || chunk_payload_bytes == 0) {
    return 0;
  }
  const auto chunk_bytes = static_cast<std::uint64_t>(chunk_payload_bytes);
  return (total_bytes / chunk_bytes) + ((total_bytes % chunk_bytes) == 0 ? 0 : 1);
}

inline bool ExecutionLargeValueReaderIsOwner(
    const ExecutionLargeValueHandle& handle,
    const ExecutionLargeValueVisibilityContext& context) noexcept {
  return context.reader_transaction_number == handle.owner_transaction_number &&
         ExecutionResourceUuidEquals(context.reader_transaction_uuid,
                                     handle.owner_transaction_uuid);
}

inline ExecutionLargeValueValidationResult ValidateExecutionLargeValueHandle(
    const ExecutionLargeValueHandle& handle,
    const ExecutionLargeValueVisibilityContext& context) {
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(handle.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionLargeValueValidationStatus::descriptor_invalid,
            descriptor_result.status};
  }
  if (!ExecutionLargeValueDescriptorFamilySupported(handle.descriptor.family)) {
    return {ExecutionLargeValueValidationStatus::descriptor_family_unsupported};
  }
  if (ExecutionDataPacketUuidIsNil(handle.value_uuid)) {
    return {ExecutionLargeValueValidationStatus::value_uuid_required};
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(handle.value_state))) {
    return {ExecutionLargeValueValidationStatus::value_state_invalid};
  }
  if (ExecutionLargeValueUsesExternalStorage(handle.kind)) {
    if (handle.value_state != ExecutionValueState::lob_handle) {
      return {ExecutionLargeValueValidationStatus::value_state_kind_mismatch};
    }
  } else if (handle.value_state != ExecutionValueState::value) {
    return {ExecutionLargeValueValidationStatus::value_state_kind_mismatch};
  }
  if (ExecutionDataPacketUuidIsNil(handle.owner_transaction_uuid)) {
    return {ExecutionLargeValueValidationStatus::owner_transaction_uuid_required};
  }
  if (handle.owner_transaction_number == 0) {
    return {ExecutionLargeValueValidationStatus::owner_transaction_number_required};
  }

  if (ExecutionLargeValueUsesExternalStorage(handle.kind)) {
    if (handle.total_bytes == 0) {
      return {ExecutionLargeValueValidationStatus::total_bytes_required};
    }
    if (handle.chunk_payload_bytes == 0) {
      return {ExecutionLargeValueValidationStatus::chunk_payload_bytes_required};
    }
    if (handle.chunk_count == 0) {
      return {ExecutionLargeValueValidationStatus::chunk_count_required};
    }
    if (handle.chunk_count != ExecutionLargeValueExpectedChunkCount(
                                  handle.total_bytes,
                                  handle.chunk_payload_bytes)) {
      return {ExecutionLargeValueValidationStatus::chunk_count_mismatch};
    }
    if (handle.content_hash.empty()) {
      return {ExecutionLargeValueValidationStatus::content_hash_required};
    }
    if (!handle.integrity_verified) {
      return {ExecutionLargeValueValidationStatus::integrity_required};
    }
    if (!handle.storage_reference_authoritative) {
      return {
          ExecutionLargeValueValidationStatus::storage_reference_not_authoritative};
    }
    if (!handle.parser_independent) {
      return {ExecutionLargeValueValidationStatus::parser_dependent};
    }
  }

  ExecutionLargeValueValidationResult result;
  switch (handle.lifecycle_state) {
    case ExecutionLargeValueLifecycleState::inline_value:
      result.visible = true;
      return result;

    case ExecutionLargeValueLifecycleState::durable_uncommitted:
      if (context.include_own_uncommitted &&
          (context.reader_transaction_number == 0 ||
           ExecutionDataPacketUuidIsNil(context.reader_transaction_uuid))) {
        return {ExecutionLargeValueValidationStatus::reader_transaction_required};
      }
      result.visible = context.include_own_uncommitted &&
                       ExecutionLargeValueReaderIsOwner(handle, context);
      return result;

    case ExecutionLargeValueLifecycleState::committed_visible:
      if (handle.committed_transaction_number == 0) {
        return {ExecutionLargeValueValidationStatus::commit_inventory_required};
      }
      if (context.visible_committed_high_watermark == 0) {
        return {ExecutionLargeValueValidationStatus::visibility_snapshot_required};
      }
      if (handle.kind == ExecutionLargeValueKind::stream_handle &&
          !handle.stream_final) {
        return {ExecutionLargeValueValidationStatus::stream_final_required};
      }
      result.visible = handle.committed_transaction_number <=
                       context.visible_committed_high_watermark;
      return result;

    case ExecutionLargeValueLifecycleState::rolled_back:
      result.visible = false;
      result.cleanup_eligible = context.cleanup_horizon_authoritative &&
          context.authoritative_cleanup_horizon_transaction_number >=
              handle.owner_transaction_number;
      return result;

    case ExecutionLargeValueLifecycleState::cleanup_pending:
    case ExecutionLargeValueLifecycleState::cleanup_reclaimed:
      if (!context.cleanup_horizon_authoritative) {
        return {
            ExecutionLargeValueValidationStatus::cleanup_horizon_not_authoritative};
      }
      if (context.authoritative_cleanup_horizon_transaction_number <
          handle.owner_transaction_number) {
        return {ExecutionLargeValueValidationStatus::cleanup_horizon_before_owner};
      }
      result.visible = false;
      result.cleanup_eligible = true;
      return result;

    case ExecutionLargeValueLifecycleState::quarantined:
      result.visible = false;
      return result;
  }

  return {ExecutionLargeValueValidationStatus::lifecycle_state_invalid};
}

// SEARCH_KEY: EDR_PROTECTED_VALUE_MASKING_CONTRACT
enum class ExecutionProtectedValuePurpose : std::uint8_t {
  storage = 0,
  result_projection = 1,
  diagnostic = 2,
  cache_key = 3
};

enum class ExecutionProtectionClass : std::uint8_t {
  masked = 0,
  encrypted = 1,
  tokenized = 2,
  redacted = 3,
  policy_guarded = 4
};

struct ExecutionProtectedValue {
  ExecutionValueState value_state = ExecutionValueState::protected_value;
  ExecutionTypeDescriptor descriptor;
  Uuid value_uuid{};
  Uuid security_policy_uuid{};
  Uuid masking_policy_uuid{};
  Uuid redaction_policy_uuid{};
  ExecutionProtectionClass protection_class =
      ExecutionProtectionClass::policy_guarded;
  ExecutionProtectedValuePurpose purpose =
      ExecutionProtectedValuePurpose::result_projection;
  std::string redaction_token;
  std::string cache_identity;
  bool descriptor_preserved = true;
  bool raw_payload_present = false;
  bool redaction_applied = true;
  bool cache_identity_descriptor_bound = true;
  bool policy_authoritative = true;
  bool parser_independent = true;
  bool diagnostic_safe = true;
};

enum class ExecutionProtectedValueValidationStatus : std::uint8_t {
  ok = 0,
  descriptor_invalid = 1,
  security_policy_uuid_missing = 2,
  value_uuid_required = 3,
  value_state_invalid = 4,
  value_state_kind_mismatch = 5,
  descriptor_not_preserved = 6,
  policy_not_authoritative = 7,
  parser_dependent = 8,
  raw_payload_leak = 9,
  redaction_required = 10,
  redaction_token_required = 11,
  diagnostic_not_safe = 12,
  cache_identity_required = 13,
  cache_identity_not_descriptor_bound = 14
};

constexpr std::string_view ExecutionProtectedValueValidationStatusName(
    ExecutionProtectedValueValidationStatus status) noexcept {
  switch (status) {
    case ExecutionProtectedValueValidationStatus::ok:
      return "ok";
    case ExecutionProtectedValueValidationStatus::descriptor_invalid:
      return "descriptor_invalid";
    case ExecutionProtectedValueValidationStatus::security_policy_uuid_missing:
      return "security_policy_uuid_missing";
    case ExecutionProtectedValueValidationStatus::value_uuid_required:
      return "value_uuid_required";
    case ExecutionProtectedValueValidationStatus::value_state_invalid:
      return "value_state_invalid";
    case ExecutionProtectedValueValidationStatus::value_state_kind_mismatch:
      return "value_state_kind_mismatch";
    case ExecutionProtectedValueValidationStatus::descriptor_not_preserved:
      return "descriptor_not_preserved";
    case ExecutionProtectedValueValidationStatus::policy_not_authoritative:
      return "policy_not_authoritative";
    case ExecutionProtectedValueValidationStatus::parser_dependent:
      return "parser_dependent";
    case ExecutionProtectedValueValidationStatus::raw_payload_leak:
      return "raw_payload_leak";
    case ExecutionProtectedValueValidationStatus::redaction_required:
      return "redaction_required";
    case ExecutionProtectedValueValidationStatus::redaction_token_required:
      return "redaction_token_required";
    case ExecutionProtectedValueValidationStatus::diagnostic_not_safe:
      return "diagnostic_not_safe";
    case ExecutionProtectedValueValidationStatus::cache_identity_required:
      return "cache_identity_required";
    case ExecutionProtectedValueValidationStatus::cache_identity_not_descriptor_bound:
      return "cache_identity_not_descriptor_bound";
  }
  return "unknown_protected_value_validation_status";
}

struct ExecutionProtectedValueValidationResult {
  ExecutionProtectedValueValidationStatus status =
      ExecutionProtectedValueValidationStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionProtectedValueValidationStatus::ok;
  }
};

inline ExecutionProtectedValueValidationResult ValidateExecutionProtectedValue(
    const ExecutionProtectedValue& value) {
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(value.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionProtectedValueValidationStatus::descriptor_invalid,
            descriptor_result.status};
  }
  if (!ExecutionTypeDescriptorHasModifierFlag(
          value.descriptor, ExecutionTypeModifierFlag::security_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(value.descriptor.security_policy_uuid) ||
      ExecutionDataPacketUuidIsNil(value.security_policy_uuid) ||
      !ExecutionResourceUuidEquals(value.descriptor.security_policy_uuid,
                                   value.security_policy_uuid)) {
    return {
        ExecutionProtectedValueValidationStatus::security_policy_uuid_missing};
  }
  if (ExecutionDataPacketUuidIsNil(value.value_uuid)) {
    return {ExecutionProtectedValueValidationStatus::value_uuid_required};
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(value.value_state))) {
    return {ExecutionProtectedValueValidationStatus::value_state_invalid};
  }
  if (value.value_state != ExecutionValueState::protected_value) {
    return {ExecutionProtectedValueValidationStatus::value_state_kind_mismatch};
  }
  if (!value.descriptor_preserved) {
    return {ExecutionProtectedValueValidationStatus::descriptor_not_preserved};
  }
  if (!value.policy_authoritative) {
    return {ExecutionProtectedValueValidationStatus::policy_not_authoritative};
  }
  if (!value.parser_independent) {
    return {ExecutionProtectedValueValidationStatus::parser_dependent};
  }
  if (value.raw_payload_present &&
      value.purpose != ExecutionProtectedValuePurpose::storage) {
    return {ExecutionProtectedValueValidationStatus::raw_payload_leak};
  }

  switch (value.purpose) {
    case ExecutionProtectedValuePurpose::storage:
      return {};

    case ExecutionProtectedValuePurpose::result_projection:
      if (!value.redaction_applied) {
        return {ExecutionProtectedValueValidationStatus::redaction_required};
      }
      if (value.redaction_token.empty()) {
        return {ExecutionProtectedValueValidationStatus::redaction_token_required};
      }
      return {};

    case ExecutionProtectedValuePurpose::diagnostic:
      if (!value.diagnostic_safe) {
        return {ExecutionProtectedValueValidationStatus::diagnostic_not_safe};
      }
      if (!value.redaction_applied) {
        return {ExecutionProtectedValueValidationStatus::redaction_required};
      }
      if (value.redaction_token.empty()) {
        return {ExecutionProtectedValueValidationStatus::redaction_token_required};
      }
      return {};

    case ExecutionProtectedValuePurpose::cache_key:
      if (value.cache_identity.empty()) {
        return {ExecutionProtectedValueValidationStatus::cache_identity_required};
      }
      if (!value.cache_identity_descriptor_bound) {
        return {
            ExecutionProtectedValueValidationStatus::
                cache_identity_not_descriptor_bound};
      }
      return {};
  }

  return {ExecutionProtectedValueValidationStatus::redaction_required};
}

// SEARCH_KEY: EDR_UDR_BRIDGE_VALUE_CONTRACT
enum class ExecutionUdrRuntimeKind : std::uint8_t {
  cpp = 0,
  non_cpp = 1,
  unknown = 0xff
};

enum class ExecutionUdrValueDirection : std::uint8_t {
  input = 0,
  output = 1,
  inout = 2
};

struct ExecutionUdrTypeMapping {
  std::string mapping_name;
  std::string mapping_version;
  ExecutionTypeDescriptor descriptor;
  bool cpp_abi = true;
  bool descriptor_preserving = true;
  bool parser_independent = true;
};

struct ExecutionUdrBridgeValue {
  ExecutionUdrRuntimeKind runtime_kind = ExecutionUdrRuntimeKind::cpp;
  ExecutionUdrValueDirection direction = ExecutionUdrValueDirection::input;
  ExecutionValueState value_state = ExecutionValueState::value;
  ExecutionTypeDescriptor descriptor;
  Uuid value_uuid{};
  ExecutionUdrTypeMapping type_mapping;
  bool descriptor_preserved = true;
  bool payload_present = true;
  bool parser_independent = true;
};

enum class ExecutionUdrBridgeValueStatus : std::uint8_t {
  ok = 0,
  descriptor_invalid = 1,
  value_uuid_required = 2,
  runtime_not_cpp = 3,
  non_cpp_runtime_prohibited = 4,
  type_mapping_required = 5,
  type_mapping_version_required = 6,
  type_mapping_descriptor_invalid = 7,
  type_mapping_descriptor_mismatch = 8,
  type_mapping_not_cpp_abi = 9,
  type_mapping_not_descriptor_preserving = 10,
  type_mapping_parser_dependent = 11,
  descriptor_not_preserved = 12,
  value_state_invalid = 13,
  input_value_state_invalid = 14,
  input_payload_required = 15,
  output_value_state_invalid = 16,
  output_payload_required = 17,
  parser_dependent = 18,
  direction_invalid = 19
};

constexpr std::string_view ExecutionUdrBridgeValueStatusName(
    ExecutionUdrBridgeValueStatus status) noexcept {
  switch (status) {
    case ExecutionUdrBridgeValueStatus::ok:
      return "ok";
    case ExecutionUdrBridgeValueStatus::descriptor_invalid:
      return "descriptor_invalid";
    case ExecutionUdrBridgeValueStatus::value_uuid_required:
      return "value_uuid_required";
    case ExecutionUdrBridgeValueStatus::runtime_not_cpp:
      return "runtime_not_cpp";
    case ExecutionUdrBridgeValueStatus::non_cpp_runtime_prohibited:
      return "non_cpp_runtime_prohibited";
    case ExecutionUdrBridgeValueStatus::type_mapping_required:
      return "type_mapping_required";
    case ExecutionUdrBridgeValueStatus::type_mapping_version_required:
      return "type_mapping_version_required";
    case ExecutionUdrBridgeValueStatus::type_mapping_descriptor_invalid:
      return "type_mapping_descriptor_invalid";
    case ExecutionUdrBridgeValueStatus::type_mapping_descriptor_mismatch:
      return "type_mapping_descriptor_mismatch";
    case ExecutionUdrBridgeValueStatus::type_mapping_not_cpp_abi:
      return "type_mapping_not_cpp_abi";
    case ExecutionUdrBridgeValueStatus::type_mapping_not_descriptor_preserving:
      return "type_mapping_not_descriptor_preserving";
    case ExecutionUdrBridgeValueStatus::type_mapping_parser_dependent:
      return "type_mapping_parser_dependent";
    case ExecutionUdrBridgeValueStatus::descriptor_not_preserved:
      return "descriptor_not_preserved";
    case ExecutionUdrBridgeValueStatus::value_state_invalid:
      return "value_state_invalid";
    case ExecutionUdrBridgeValueStatus::input_value_state_invalid:
      return "input_value_state_invalid";
    case ExecutionUdrBridgeValueStatus::input_payload_required:
      return "input_payload_required";
    case ExecutionUdrBridgeValueStatus::output_value_state_invalid:
      return "output_value_state_invalid";
    case ExecutionUdrBridgeValueStatus::output_payload_required:
      return "output_payload_required";
    case ExecutionUdrBridgeValueStatus::parser_dependent:
      return "parser_dependent";
    case ExecutionUdrBridgeValueStatus::direction_invalid:
      return "direction_invalid";
  }
  return "unknown_udr_bridge_value_status";
}

struct ExecutionUdrBridgeValueValidationResult {
  ExecutionUdrBridgeValueStatus status = ExecutionUdrBridgeValueStatus::ok;
  ExecutionDataPacketStatus descriptor_status = ExecutionDataPacketStatus::ok;
  ExecutionDataPacketStatus type_mapping_descriptor_status =
      ExecutionDataPacketStatus::ok;

  bool ok() const noexcept {
    return status == ExecutionUdrBridgeValueStatus::ok;
  }
};

inline bool ExecutionUdrBridgeInputStateAllowed(
    ExecutionValueState state) noexcept {
  return state == ExecutionValueState::value ||
         state == ExecutionValueState::sql_null ||
         state == ExecutionValueState::lob_handle ||
         state == ExecutionValueState::protected_value;
}

inline bool ExecutionUdrBridgeOutputStateAllowed(
    ExecutionValueState state) noexcept {
  return state == ExecutionValueState::value ||
         state == ExecutionValueState::sql_null ||
         state == ExecutionValueState::lob_handle ||
         state == ExecutionValueState::protected_value ||
         state == ExecutionValueState::error;
}

inline ExecutionUdrBridgeValueValidationResult ValidateExecutionUdrBridgeValue(
    const ExecutionUdrBridgeValue& value) {
  const auto descriptor_result =
      ValidateExecutionDataPacketDescriptor(value.descriptor, 0);
  if (!descriptor_result.ok()) {
    return {ExecutionUdrBridgeValueStatus::descriptor_invalid,
            descriptor_result.status};
  }
  if (ExecutionDataPacketUuidIsNil(value.value_uuid)) {
    return {ExecutionUdrBridgeValueStatus::value_uuid_required};
  }
  if (value.runtime_kind == ExecutionUdrRuntimeKind::non_cpp) {
    return {ExecutionUdrBridgeValueStatus::non_cpp_runtime_prohibited};
  }
  if (value.runtime_kind != ExecutionUdrRuntimeKind::cpp) {
    return {ExecutionUdrBridgeValueStatus::runtime_not_cpp};
  }
  if (value.type_mapping.mapping_name.empty()) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_required};
  }
  if (value.type_mapping.mapping_version.empty()) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_version_required};
  }
  const auto mapping_descriptor_result =
      ValidateExecutionDataPacketDescriptor(value.type_mapping.descriptor, 1);
  if (!mapping_descriptor_result.ok()) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_descriptor_invalid,
            ExecutionDataPacketStatus::ok, mapping_descriptor_result.status};
  }
  if (!ExecutionTypeDescriptorIdentityEquals(value.descriptor,
                                             value.type_mapping.descriptor)) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_descriptor_mismatch};
  }
  if (!value.type_mapping.cpp_abi) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_not_cpp_abi};
  }
  if (!value.type_mapping.descriptor_preserving) {
    return {
        ExecutionUdrBridgeValueStatus::type_mapping_not_descriptor_preserving};
  }
  if (!value.type_mapping.parser_independent) {
    return {ExecutionUdrBridgeValueStatus::type_mapping_parser_dependent};
  }
  if (!value.descriptor_preserved) {
    return {ExecutionUdrBridgeValueStatus::descriptor_not_preserved};
  }
  if (!PlainValuePayloadStateCodeIsValid(
          static_cast<std::uint8_t>(value.value_state))) {
    return {ExecutionUdrBridgeValueStatus::value_state_invalid};
  }
  if (!value.parser_independent) {
    return {ExecutionUdrBridgeValueStatus::parser_dependent};
  }

  switch (value.direction) {
    case ExecutionUdrValueDirection::input:
    case ExecutionUdrValueDirection::inout:
      if (!ExecutionUdrBridgeInputStateAllowed(value.value_state)) {
        return {ExecutionUdrBridgeValueStatus::input_value_state_invalid};
      }
      if (ExecutionValueStateHasPayload(value.value_state) &&
          !value.payload_present) {
        return {ExecutionUdrBridgeValueStatus::input_payload_required};
      }
      return {};

    case ExecutionUdrValueDirection::output:
      if (!ExecutionUdrBridgeOutputStateAllowed(value.value_state)) {
        return {ExecutionUdrBridgeValueStatus::output_value_state_invalid};
      }
      if (ExecutionValueStateHasPayload(value.value_state) &&
          !value.payload_present) {
        return {ExecutionUdrBridgeValueStatus::output_payload_required};
      }
      return {};
  }

  return {ExecutionUdrBridgeValueStatus::direction_invalid};
}

}  // namespace scratchbird::engine
