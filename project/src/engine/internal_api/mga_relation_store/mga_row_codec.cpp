// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_row_codec.hpp"

#include "crud_support/crud_store.hpp"
#include "catalog/column_metadata_codec.hpp"
#include <stdexcept>
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_ROW_CODEC_IMPLEMENTATION_AUTHORITY
// Owns text/binary row-version framing, typed/native value materialization,
// bounded decode accounting, and format validation. Decoded creator/event
// identities are data only; MGA inventory and snapshots decide visibility.

namespace idx = scratchbird::core::index;

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";

bool InspectBinarySegment(const std::string& path, bool* present) {
  *present = false;
  std::error_code error;
  const auto link_status = std::filesystem::symlink_status(path, error);
  if (link_status.type() == std::filesystem::file_type::not_found &&
      (!error || error == std::errc::no_such_file_or_directory)) return true;
  if (error) return false;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) return false;
  *present = true;
  return true;
}

}  // namespace

bool ReadCompleteMgaBinaryFile(const std::string& path,
                              std::vector<idx::byte>* bytes,
                              const std::uint64_t maximum_bytes) {
  if (bytes == nullptr) return false;
  bytes->clear();
  bool present = false;
  if (!InspectBinarySegment(path, &present)) return false;
  if (!present) return true;
  std::error_code error;
  const auto expected_size = std::filesystem::file_size(path, error);
  if (error || expected_size > bytes->max_size() || expected_size > maximum_bytes) return false;
  const auto expected_time = std::filesystem::last_write_time(path, error);
  if (error) return false;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open() || !input.good()) return false;
  std::vector<idx::byte> staged;
  std::array<char, 64 * 1024> chunk{};
  for (;;) {
    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto count = input.gcount();
    if (input.bad() || count < 0 ||
        static_cast<std::uintmax_t>(count) > expected_size - staged.size()) return false;
    staged.insert(staged.end(), chunk.data(), chunk.data() + count);
    if (input.eof()) break;
    if (input.fail()) return false;
  }
  if (staged.size() != expected_size ||
      !InspectBinarySegment(path, &present) || !present) return false;
  const auto final_size = std::filesystem::file_size(path, error);
  if (error || final_size != expected_size) return false;
  const auto final_time = std::filesystem::last_write_time(path, error);
  if (error || final_time != expected_time) return false;
  *bytes = std::move(staged);
  return true;
}

void ReserveAmortizedAppendCapacity(std::string* out, std::size_t extra);

std::uint64_t ReadLittleEndianU64(std::string_view payload) {
  std::uint64_t value = 0;
  const std::size_t bytes = std::min<std::size_t>(payload.size(), 8);
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(payload[index]))
             << (index * 8u);
  }
  return value;
}

std::uint32_t ReadLittleEndianU32(std::string_view payload) {
  std::uint32_t value = 0;
  const std::size_t bytes = std::min<std::size_t>(payload.size(), 4);
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(payload[index]))
             << (index * 8u);
  }
  return value;
}

std::string Int64ToStringFast(std::int64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string UInt64ToStringFast(std::uint64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string Real64ToStringFast(double value) {
  char buffer[64] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string ScopedRowBinaryMaterializeValue(std::string_view type_name,
                                            std::string_view payload) {
  if (type_name == "boolean" && payload.size() == 1) {
    return payload[0] == 0 ? "false" : "true";
  }
  if (type_name == "int32" && payload.size() == 4) {
    const std::uint32_t bits = ReadLittleEndianU32(payload);
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return Int64ToStringFast(value);
  }
  if (type_name == "int64" && payload.size() == 8) {
    const std::uint64_t bits = ReadLittleEndianU64(payload);
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return Int64ToStringFast(value);
  }
  if (type_name == "uint64" && payload.size() == 8) {
    return UInt64ToStringFast(ReadLittleEndianU64(payload));
  }
  if (type_name == "real64" && payload.size() == 8) {
    const std::uint64_t bits = ReadLittleEndianU64(payload);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return Real64ToStringFast(value);
  }
  return std::string(payload);
}

std::string_view ScopedRowNativePacketTypeName(std::uint8_t tag) {
  switch (tag) {
    case 1: return "text";
    case 2: return "int64";
    case 3: return "boolean";
    case 4: return "int32";
    case 5: return "uint64";
    case 6: return "real64";
    case 7: return "binary";
    default: return {};
  }
}

std::string ScopedRowBinaryTypeName(const EngineTypedValue& typed) {
  if (!typed.descriptor.canonical_type_name.empty()) {
    return typed.descriptor.canonical_type_name;
  }
  CatalogColumnMetadata metadata;
  if (AdmitCatalogColumnMetadata(typed.descriptor.encoded_descriptor, &metadata)) {
    for (const char* key : {"type", "canonical"}) {
      const auto it = metadata.text.find(key);
      if (it != metadata.text.end()) return it->second;
    }
  }
  return "text";
}

std::string_view ScopedRowBinaryPayloadView(const EngineTypedValue& typed) {
  if (!typed.binary_value.empty()) {
    return std::string_view(
        reinterpret_cast<const char*>(typed.binary_value.data()),
        typed.binary_value.size());
  }
  return std::string_view(typed.encoded_value);
}

bool ScopedRowBinaryCanonicalPayload(const EngineTypedValue& typed,
                                     std::string* payload) {
  if (payload == nullptr || typed.isSqlNull()) {
    return false;
  }
  payload->clear();
  const std::string type_name = ScopedRowBinaryTypeName(typed);
  if (type_name == "uuid") {
    if (!typed.encoded_value.empty() || typed.binary_value.size() != 16) return false;
    payload->assign(reinterpret_cast<const char*>(typed.binary_value.data()), 16);
    return true;
  }
  const auto append_little_endian = [&](std::uint64_t value,
                                        std::size_t width) {
    payload->reserve(width);
    for (std::size_t byte = 0; byte < width; ++byte) {
      payload->push_back(static_cast<char>((value >> (byte * 8u)) & 0xffu));
    }
  };
  const auto required_binary_width = [&]() -> std::size_t {
    if (type_name == "boolean") return 1;
    if (type_name == "int32") return 4;
    if (type_name == "int64" || type_name == "uint64" ||
        type_name == "real64") {
      return 8;
    }
    return 0;
  }();
  if (!typed.binary_value.empty()) {
    if (required_binary_width != 0 &&
        typed.binary_value.size() != required_binary_width) {
      return false;
    }
    payload->assign(
        reinterpret_cast<const char*>(typed.binary_value.data()),
        typed.binary_value.size());
    return true;
  }

  const std::string_view text = typed.encoded_value;
  if (type_name == "boolean") {
    if (text == "true" || text == "1") {
      payload->push_back('\x01');
      return true;
    }
    if (text == "false" || text == "0") {
      payload->push_back('\x00');
      return true;
    }
    return false;
  }
  if (type_name == "int32") {
    std::int32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, 10);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
      return false;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_little_endian(bits, sizeof(bits));
    return true;
  }
  if (type_name == "int64") {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, 10);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
      return false;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_little_endian(bits, sizeof(bits));
    return true;
  }
  if (type_name == "uint64") {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value, 10);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
      return false;
    }
    append_little_endian(value, sizeof(value));
    return true;
  }
  if (type_name == "real64") {
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || !std::isfinite(value)) {
      return false;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_little_endian(bits, sizeof(bits));
    return true;
  }

  payload->assign(text.data(), text.size());
  return true;
}

std::size_t ScopedRowBinaryBatchEstimateBytes(
    const std::vector<CrudRowVersionRecord>& rows,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    bool compact_batch) {
  std::size_t bytes = kScopedRowBinaryBatchMagic.size() + 2 + 2 + 4 + 8;
  for (const auto& field : field_order) {
    bytes += 4 + field.size();
  }
  if (!typed_rows.empty()) {
    for (const auto& [_, typed] : typed_rows.front().fields) {
      const std::string type_name = ScopedRowBinaryTypeName(typed);
      bytes += 4 + type_name.size();
    }
  }
  if (compact_batch) {
    bytes += 8 + 8 + rows.front().table_uuid.bytes.size() +
             rows.front().temporary_session_uuid.bytes.size() + 1;
  }
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    if (compact_batch) {
      bytes += 16 + 16;
    } else {
      bytes += 8 + 8 + 8 + 1;
      bytes += row.table_uuid.bytes.size();
      bytes += row.row_uuid.bytes.size();
      bytes += row.version_uuid.bytes.size();
      bytes += row.previous_version_uuid.bytes.size();
      bytes += row.temporary_session_uuid.bytes.size();
    }
    bytes += null_bitmap_bytes;
    const auto& typed_row = typed_rows[index];
    for (const auto& [_, typed] : typed_row.fields) {
      if (typed.isSqlNull()) { continue; }
      bytes += 4 + ScopedRowBinaryPayloadView(typed).size();
    }
  }
  return bytes;
}

bool ScopedRowBinaryCompactBatchEligible(
    const std::vector<CrudRowVersionRecord>& rows) {
  (void)rows;
  return false;
}

bool AppendScopedRowBinaryBatch(std::string* out,
                                const std::vector<CrudRowVersionRecord>& rows,
                                std::span<const EngineRowValue> typed_rows,
                                std::span<const std::string> field_order,
                                std::uint64_t first_event_sequence) {
  if (out == nullptr || rows.empty() || typed_rows.size() != rows.size() ||
      (field_order.empty() && !std::ranges::all_of(rows, [](const auto& row) { return row.deleted; })) ||
      rows.size() > std::numeric_limits<std::uint64_t>::max() ||
      field_order.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const bool compact_batch = ScopedRowBinaryCompactBatchEligible(rows);
  ReserveAmortizedAppendCapacity(
      out,
      ScopedRowBinaryBatchEstimateBytes(rows,
                                        typed_rows,
                                        field_order,
                                        compact_batch));
  out->append(kScopedRowBinaryBatchMagic.data(),
              kScopedRowBinaryBatchMagic.size());
  AppendBinaryU16(out,
                  compact_batch ? kScopedRowBinaryVersion
                                : kScopedRowBinaryGeneralVersion);
  AppendBinaryU16(out, 0);
  AppendBinaryU32(out, static_cast<std::uint32_t>(field_order.size()));
  AppendBinaryU64(out, static_cast<std::uint64_t>(rows.size()));
  for (const auto& field : field_order) {
    if (!AppendBinaryString(out, field)) { return false; }
  }
  if (typed_rows.empty() || typed_rows.front().fields.size() != field_order.size()) {
    return false;
  }
  for (const auto& [_, typed] : typed_rows.front().fields) {
    if (!AppendBinaryString(out, ScopedRowBinaryTypeName(typed))) {
      return false;
    }
  }
  if (compact_batch) {
    AppendBinaryU64(out, first_event_sequence);
    AppendBinaryU64(out, rows.front().creator_tx);
    if (!AppendBinaryEngineUuid(out, rows.front().table_uuid) ||
        !AppendBinaryEngineUuid(out, rows.front().temporary_session_uuid, true)) {
      return false;
    }
    AppendBinaryU8(out, 0);
  }
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  std::vector<std::uint8_t> null_bitmap(null_bitmap_bytes);
  std::uint64_t event_sequence = first_event_sequence;
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    const auto& row = rows[row_index];
    const auto& typed_row = typed_rows[row_index];
    if (typed_row.fields.size() != field_order.size()) { return false; }
    if (compact_batch) {
      if (!AppendBinaryEngineUuid(out, row.row_uuid) ||
          !AppendBinaryEngineUuid(out, row.version_uuid)) {
        return false;
      }
      ++event_sequence;
    } else {
      AppendBinaryU64(out, row.creator_tx);
      AppendBinaryU64(out, event_sequence++);
      AppendBinaryU64(out, row.previous_sequence);
      AppendBinaryU8(out, row.deleted ? 1u : 0u);
      if (!AppendBinaryEngineUuid(out, row.table_uuid) ||
          !AppendBinaryEngineUuid(out, row.row_uuid) ||
          !AppendBinaryEngineUuid(out, row.version_uuid) ||
          !AppendBinaryEngineUuid(out, row.previous_version_uuid, true) ||
          !AppendBinaryEngineUuid(out, row.temporary_session_uuid, true)) {
        return false;
      }
    }
    std::fill(null_bitmap.begin(), null_bitmap.end(), 0);
    for (std::size_t field_index = 0; field_index < typed_row.fields.size();
         ++field_index) {
      if (typed_row.fields[field_index].second.isSqlNull()) {
        null_bitmap[field_index / 8u] |=
            static_cast<std::uint8_t>(1u << (field_index % 8u));
      }
    }
    for (const std::uint8_t byte : null_bitmap) {
      AppendBinaryU8(out, byte);
    }
    for (const auto& [_, typed] : typed_row.fields) {
      if (typed.isSqlNull()) { continue; }
      std::string canonical_payload;
      if (!ScopedRowBinaryCanonicalPayload(typed, &canonical_payload) ||
          !AppendBinaryString(out, canonical_payload)) {
        return false;
      }
    }
  }
  return true;
}

std::size_t ScopedRowBinaryIdentityBatchEstimateBytes(
    const std::vector<CrudRowVersionRecord>& rows,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    const EngineUuid& table_uuid,
    const EngineUuid& temporary_session_uuid) {
  std::size_t bytes = kScopedRowBinaryBatchMagic.size() + 2 + 2 + 4 + 8;
  for (const auto& field : field_order) {
    bytes += 4 + field.size();
  }
  if (!typed_rows.empty()) {
    for (const auto& [_, typed] : typed_rows.front().fields) {
      const std::string type_name = ScopedRowBinaryTypeName(typed);
      bytes += 4 + type_name.size();
    }
  }
  bytes += 8 + 8 + table_uuid.bytes.size() +
           temporary_session_uuid.bytes.size() + 1;
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    bytes += 16 + 16;
    bytes += null_bitmap_bytes;
    const auto& typed_row = typed_rows[index];
    for (const auto& [_, typed] : typed_row.fields) {
      if (typed.isSqlNull()) { continue; }
      bytes += 4 + ScopedRowBinaryPayloadView(typed).size();
    }
  }
  return bytes;
}

bool AppendScopedRowIdentityBinaryBatch(
    std::string* out,
    const std::vector<CrudRowVersionRecord>& row_identities,
    const EngineUuid& table_uuid,
    const EngineUuid& temporary_session_uuid,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || row_identities.empty() ||
      typed_rows.size() != row_identities.size() ||
      table_uuid.is_nil() || field_order.empty() ||
      row_identities.size() > std::numeric_limits<std::uint64_t>::max() ||
      field_order.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  ReserveAmortizedAppendCapacity(
      out,
      ScopedRowBinaryIdentityBatchEstimateBytes(row_identities,
                                                typed_rows,
                                                field_order,
                                                table_uuid,
                                                temporary_session_uuid));
  out->append(kScopedRowBinaryBatchMagic.data(),
              kScopedRowBinaryBatchMagic.size());
  AppendBinaryU16(out, kScopedRowBinaryVersion);
  AppendBinaryU16(out, 0);
  AppendBinaryU32(out, static_cast<std::uint32_t>(field_order.size()));
  AppendBinaryU64(out, static_cast<std::uint64_t>(row_identities.size()));
  for (const auto& field : field_order) {
    if (!AppendBinaryString(out, field)) { return false; }
  }
  if (typed_rows.empty() ||
      typed_rows.front().fields.size() != field_order.size()) {
    return false;
  }
  for (const auto& [_, typed] : typed_rows.front().fields) {
    if (!AppendBinaryString(out, ScopedRowBinaryTypeName(typed))) {
      return false;
    }
  }
  AppendBinaryU64(out, first_event_sequence);
  AppendBinaryU64(out, creator_tx);
  if (!AppendBinaryEngineUuid(out, table_uuid) ||
      !AppendBinaryEngineUuid(out, temporary_session_uuid, true)) {
    return false;
  }
  AppendBinaryU8(out, 0);
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  std::vector<std::uint8_t> null_bitmap(null_bitmap_bytes);
  for (std::size_t row_index = 0; row_index < row_identities.size();
       ++row_index) {
    const auto& row = row_identities[row_index];
    const auto& typed_row = typed_rows[row_index];
    if (row.row_uuid.is_nil() || row.version_uuid.is_nil() ||
        typed_row.fields.size() != field_order.size()) {
      return false;
    }
    if (!AppendBinaryEngineUuid(out, row.row_uuid) ||
        !AppendBinaryEngineUuid(out, row.version_uuid)) {
      return false;
    }
    std::fill(null_bitmap.begin(), null_bitmap.end(), 0);
    for (std::size_t field_index = 0; field_index < typed_row.fields.size();
         ++field_index) {
      if (typed_row.fields[field_index].second.isSqlNull()) {
        null_bitmap[field_index / 8u] |=
            static_cast<std::uint8_t>(1u << (field_index % 8u));
      }
    }
    for (const std::uint8_t byte : null_bitmap) {
      AppendBinaryU8(out, byte);
    }
    for (const auto& [_, typed] : typed_row.fields) {
      if (typed.isSqlNull()) { continue; }
      std::string canonical_payload;
      if (!ScopedRowBinaryCanonicalPayload(typed, &canonical_payload) ||
          !AppendBinaryString(out, canonical_payload)) {
        return false;
      }
    }
  }
  return true;
}

std::size_t ScopedRowNativePacketIdentityBatchEstimateBytes(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const EngineNativeRowPacketFrame& frame,
    const EngineUuid& table_uuid,
    const EngineUuid& temporary_session_uuid) {
  std::size_t bytes = kScopedRowBinaryBatchMagic.size() + 2 + 2 + 4 + 8;
  for (const auto& field : frame.field_order) {
    bytes += 4 + field.size();
  }
  bytes += frame.column_type_tags.size();
  bytes += 8 + 8 + table_uuid.bytes.size() +
           temporary_session_uuid.bytes.size() + 1;
  for (std::size_t index = 0; index < row_identities.size() &&
                              index < frame.row_sizes.size();
       ++index) {
    bytes += 16 + 16;
    bytes += frame.row_sizes[index];
  }
  return bytes;
}

bool AppendScopedRowIdentityNativePacketBatch(
    std::string* out,
    const std::vector<CrudRowVersionRecord>& row_identities,
    const EngineUuid& table_uuid,
    const EngineUuid& temporary_session_uuid,
    const EngineNativeRowPacketFrame& frame,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || row_identities.empty() || !frame.present ||
      frame.version != 2 ||
      table_uuid.is_nil() ||
      frame.row_count != row_identities.size() ||
      frame.column_count == 0 ||
      frame.field_order.size() != frame.column_count ||
      frame.column_type_tags.size() != frame.column_count ||
      frame.row_offsets.size() != row_identities.size() ||
      frame.row_sizes.size() != row_identities.size() ||
      frame.packet_bytes.empty() ||
      row_identities.size() > std::numeric_limits<std::uint64_t>::max() ||
      frame.field_order.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  ReserveAmortizedAppendCapacity(
      out,
      ScopedRowNativePacketIdentityBatchEstimateBytes(row_identities,
                                                      frame,
                                                      table_uuid,
                                                      temporary_session_uuid));
  out->append(kScopedRowBinaryBatchMagic.data(),
              kScopedRowBinaryBatchMagic.size());
  AppendBinaryU16(out, kScopedRowBinaryNativePacketVersion);
  AppendBinaryU16(out, 0);
  AppendBinaryU32(out, static_cast<std::uint32_t>(frame.field_order.size()));
  AppendBinaryU64(out, static_cast<std::uint64_t>(row_identities.size()));
  for (const auto& field : frame.field_order) {
    if (!AppendBinaryString(out, field)) { return false; }
  }
  for (const std::uint8_t tag : frame.column_type_tags) {
    AppendBinaryU8(out, tag);
  }
  AppendBinaryU64(out, first_event_sequence);
  AppendBinaryU64(out, creator_tx);
  if (!AppendBinaryEngineUuid(out, table_uuid) ||
      !AppendBinaryEngineUuid(out, temporary_session_uuid, true)) {
    return false;
  }
  AppendBinaryU8(out, 0);
  for (std::size_t row_index = 0; row_index < row_identities.size();
       ++row_index) {
    const auto& row = row_identities[row_index];
    if (row.row_uuid.is_nil() || row.version_uuid.is_nil()) {
      return false;
    }
    const std::size_t row_offset = frame.row_offsets[row_index];
    const std::size_t row_size = frame.row_sizes[row_index];
    if (row_size == 0 || row_offset > frame.packet_bytes.size() ||
        row_size > frame.packet_bytes.size() - row_offset) {
      return false;
    }
    if (!AppendBinaryEngineUuid(out, row.row_uuid) ||
        !AppendBinaryEngineUuid(out, row.version_uuid)) {
      return false;
    }
    out->append(reinterpret_cast<const char*>(frame.packet_bytes.data() +
                                             row_offset),
                row_size);
  }
  return true;
}


bool ObserveBoundedHeapReadMemory(BoundedScopedRowReadControl* control,
                                  const std::uint64_t live_bytes) {
  if (control == nullptr || control->maximum_memory_bytes == 0) return true;
  control->peak_live_memory_bytes =
      std::max(control->peak_live_memory_bytes, live_bytes);
  if (live_bytes <= control->maximum_memory_bytes) return true;
  control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
  control->refusal_detail = "heap_read_maximum_memory_bytes_exceeded";
  return false;
}

bool AccountHeapReadWait(
    BoundedScopedRowReadControl* control,
    const std::chrono::steady_clock::time_point started) {
  if (control == nullptr || control->runtime_observation == nullptr) {
    return true;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  if (elapsed < 0 || static_cast<std::uintmax_t>(elapsed) >
                         std::numeric_limits<std::uint64_t>::max() ||
      static_cast<std::uint64_t>(elapsed) >
          std::numeric_limits<std::uint64_t>::max() -
              control->runtime_observation->operator_wait_ns) {
    control->runtime_observation->complete = false;
    control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
    control->refusal_detail = "heap_read_wait_observation_overflow";
    return false;
  }
  control->runtime_observation->operator_wait_ns +=
      static_cast<std::uint64_t>(elapsed);
  return true;
}

bool AccountHeapStorageBytes(BoundedScopedRowReadControl* control,
                             const std::uint64_t bytes) {
  if (control == nullptr || control->runtime_observation == nullptr) {
    return true;
  }
  if (bytes > std::numeric_limits<std::uint64_t>::max() -
                  control->runtime_observation->storage_bytes_read) {
    control->runtime_observation->complete = false;
    control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
    control->refusal_detail = "heap_read_storage_byte_observation_overflow";
    return false;
  }
  control->runtime_observation->storage_bytes_read += bytes;
  return true;
}

bool BoundedScopedReadCancelled(BoundedScopedRowReadControl* control) {
  if (control == nullptr || control->cancellation_requested == nullptr ||
      !*control->cancellation_requested) {
    return false;
  }
  if (!(*control->cancellation_requested)()) { return false; }
  control->cancellation_observed = true;
  control->failure_category = MgaHeapReadFailureCategoryV1::kCancellation;
  control->refusal_detail = "heap_read_cancelled";
  return true;
}

bool AccountBoundedScopedBytes(BoundedScopedRowReadControl* control,
                               const std::uint64_t bytes) {
  if (control == nullptr) { return true; }
  if (control->decoded_bytes > control->maximum_bytes ||
      bytes > control->maximum_bytes - control->decoded_bytes) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
    control->refusal_detail = "heap_read_maximum_decoded_bytes_exceeded";
    return false;
  }
  control->decoded_bytes += bytes;
  return true;
}

bool ReadBinaryFileBounded(const std::string& path,
                           const std::uint64_t authorized_file_bytes,
                           BoundedScopedRowReadControl* control,
                           std::vector<idx::byte>* bytes) {
  if (control == nullptr || bytes == nullptr ||
      authorized_file_bytes >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    if (control != nullptr) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
      control->refusal_detail = "heap_read_scoped_binary_size_overflow";
    }
    return false;
  }
  std::ifstream input;
  const auto open_started = std::chrono::steady_clock::now();
  input.open(path, std::ios::binary);
  if (!AccountHeapReadWait(control, open_started)) return false;
  if (!input) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_scoped_binary_open_failed";
    return false;
  }
  bytes->clear();
  std::uint64_t binary_phase_bytes = control->retained_parent_memory_bytes;
  std::uint64_t binary_allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          authorized_file_bytes, sizeof(idx::byte),
          &binary_allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(control->retained_decode_row_memory_bytes,
                                &binary_phase_bytes) ||
      !CheckedHeapReadMemoryAdd(sizeof(*bytes), &binary_phase_bytes) ||
      !CheckedHeapReadMemoryAdd(binary_allocation_bytes,
                                &binary_phase_bytes) ||
      !ObserveBoundedHeapReadMemory(control, binary_phase_bytes)) {
    if (control->refusal_detail.empty()) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
      control->refusal_detail = "heap_read_binary_memory_receipt_overflow";
    }
    return false;
  }
  bytes->reserve(static_cast<std::size_t>(authorized_file_bytes));
  constexpr std::size_t kReadChunkBytes = 64 * 1024;
  char chunk[kReadChunkBytes];
  std::uint64_t actual_file_bytes = 0;
  while (true) {
    if (BoundedScopedReadCancelled(control)) { return false; }
    const std::uint64_t remaining =
        authorized_file_bytes - actual_file_bytes;
    const std::size_t requested = remaining == 0
                                      ? 1
                                      : static_cast<std::size_t>(std::min<
                                            std::uint64_t>(remaining,
                                                           kReadChunkBytes));
    const auto read_started = std::chrono::steady_clock::now();
    input.read(chunk, static_cast<std::streamsize>(requested));
    if (!AccountHeapReadWait(control, read_started)) return false;
    const std::streamsize read_count = input.gcount();
    if (read_count < 0 ||
        static_cast<std::uint64_t>(read_count) > remaining) {
      control->refusal_detail = "heap_read_scoped_binary_grew_during_read";
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      return false;
    }
    if (read_count > 0) {
      const std::size_t old_size = bytes->size();
      const std::size_t appended = static_cast<std::size_t>(read_count);
      if (appended > std::numeric_limits<std::size_t>::max() - old_size) {
        control->refusal_detail = "heap_read_scoped_binary_size_overflow";
        control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
        return false;
      }
      bytes->resize(old_size + appended);
      std::memcpy(bytes->data() + old_size, chunk, appended);
      actual_file_bytes += static_cast<std::uint64_t>(read_count);
      if (!AccountHeapStorageBytes(
              control, static_cast<std::uint64_t>(read_count))) {
        return false;
      }
    }
    if (input.bad()) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_scoped_binary_read_failed";
      return false;
    }
    if (read_count < static_cast<std::streamsize>(requested)) {
      if (input.eof()) {
        if (actual_file_bytes != authorized_file_bytes) {
          control->refusal_detail =
              "heap_read_scoped_binary_changed_during_read";
          control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
          return false;
        }
        return true;
      }
      control->refusal_detail = "heap_read_scoped_binary_read_failed";
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      return false;
    }
  }
}

bool AdmitBoundedScopedRow(BoundedScopedRowReadControl* control,
                           const CrudRowVersionRecord& row) {
  if (control == nullptr) { return true; }
  if (BoundedScopedReadCancelled(control)) { return false; }
  if (control->decoded_row_versions >= control->maximum_row_versions) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
    control->refusal_detail = "heap_read_maximum_row_versions_exceeded";
    return false;
  }
  std::uint64_t decoded_bytes = 0;
  const auto add_decoded_size = [&](const std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - decoded_bytes) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
      control->refusal_detail = "heap_read_decoded_byte_count_overflow";
      return false;
    }
    decoded_bytes += static_cast<std::uint64_t>(bytes);
    return true;
  };
  if (!add_decoded_size(row.table_uuid.bytes.size()) ||
      !add_decoded_size(row.row_uuid.bytes.size()) ||
      !add_decoded_size(row.version_uuid.bytes.size()) ||
      !add_decoded_size(row.temporary_session_uuid.bytes.size()) ||
      !add_decoded_size(row.previous_version_uuid.bytes.size())) {
    return false;
  }
  for (const auto& [key, value] : row.values) {
    if (!add_decoded_size(key.size()) || !add_decoded_size(value.size())) {
      return false;
    }
  }
  if (!AccountBoundedScopedBytes(control, decoded_bytes)) { return false; }
  if (control->runtime_observation != nullptr) {
    if (decoded_bytes > std::numeric_limits<std::uint64_t>::max() -
                            control->runtime_observation->decoded_bytes) {
      control->runtime_observation->complete = false;
      control->failure_category = MgaHeapReadFailureCategoryV1::kResource;
      control->refusal_detail = "heap_read_decode_observation_overflow";
      return false;
    }
    control->runtime_observation->decoded_bytes += decoded_bytes;
  }
  ++control->decoded_row_versions;
  return true;
}

bool DecodeScopedRowBinaryStore(
    const std::string& path,
    std::vector<CrudRowVersionRecord>* rows,
    ScopedRelationSummary* summary,
    BoundedScopedRowReadControl* control,
    const std::uint64_t authorized_file_bytes) {
  if (rows == nullptr || summary == nullptr) { return false; }
  summary->trusted = false;
  if (BoundedScopedReadCancelled(control)) { return false; }
  std::vector<idx::byte> bytes;
  if (control == nullptr) {
    if (!ReadCompleteMgaBinaryFile(path, &bytes)) return false;
  } else {
    const auto existence_started = std::chrono::steady_clock::now();
    bool file_exists = false;
    const bool inspected = InspectBinarySegment(path, &file_exists);
    if (!AccountHeapReadWait(control, existence_started)) return false;
    if (!inspected) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_scoped_binary_status_failed";
      return false;
    }
    if (!file_exists) {
      if (authorized_file_bytes != 0) {
        control->failure_category = MgaHeapReadFailureCategoryV1::kCorruptStorage;
        control->refusal_detail = "heap_read_scoped_binary_changed_during_read";
        return false;
      }
      summary->trusted = true;
      return true;
    }
    if (!ReadBinaryFileBounded(path, authorized_file_bytes, control, &bytes)) return false;
  }
  return DecodeScopedRowBinaryBytes(bytes, rows, summary, control);
}

bool DecodeScopedRowBinaryBytes(
    const std::vector<idx::byte>& bytes,
    std::vector<CrudRowVersionRecord>* rows,
    ScopedRelationSummary* summary,
    BoundedScopedRowReadControl* control) {
  if (rows == nullptr || summary == nullptr) return false;
  bool published = false;
  struct RestorePartialDecode {
    std::vector<CrudRowVersionRecord>& rows;
    ScopedRelationSummary& summary;
    const std::size_t original_size;
    const ScopedRelationSummary original_summary;
    const bool& published;
    ~RestorePartialDecode() {
      if (published) return;
      rows.resize(original_size);
      const bool malformed = summary.malformed;
      summary = original_summary;
      summary.trusted = false;
      summary.malformed = summary.malformed || malformed;
    }
  } restore{*rows, *summary, rows->size(), *summary, published};
  summary->trusted = false;
  if (BoundedScopedReadCancelled(control)) return false;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (BoundedScopedReadCancelled(control)) { return false; }
    if (offset + kScopedRowBinaryBatchMagic.size() > bytes.size() ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + offset),
                         kScopedRowBinaryBatchMagic.size()) !=
            kScopedRowBinaryBatchMagic) {
      summary->malformed = true;
      summary->trusted = false;
      return false;
    }
    offset += kScopedRowBinaryBatchMagic.size();
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint32_t column_count = 0;
    std::uint64_t row_count = 0;
    if (!ReadBinaryU16(bytes, &offset, &version) ||
        !ReadBinaryU16(bytes, &offset, &flags) ||
        !ReadBinaryU32(bytes, &offset, &column_count) ||
        !ReadBinaryU64(bytes, &offset, &row_count) ||
        (version != kScopedRowBinaryGeneralVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 ||
        (column_count == 0 && version != kScopedRowBinaryGeneralVersion) || column_count > 4096) {
      summary->malformed = true;
      summary->trusted = false;
      return false;
    }
    if (row_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() /
                                   std::max<std::uint32_t>(column_count, 1))) {
      summary->malformed = true;
      summary->trusted = false;
      return false;
    }
    if (control != nullptr && control->maximum_memory_bytes != 0) {
      std::uint64_t field_projection = 0;
      std::uint64_t field_structural_bytes = 0;
      std::uint64_t remaining_projection = 0;
      std::uint64_t field_phase_memory =
          control->retained_parent_memory_bytes;
      std::uint64_t binary_bytes = 0;
      if (!CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(column_count),
              2 * sizeof(std::string) + sizeof(std::uint8_t),
              &field_structural_bytes) ||
          !CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(bytes.size() - offset), 4,
              &remaining_projection) ||
          !CheckedHeapReadMemoryAdd(field_structural_bytes,
                                    &field_projection) ||
          !CheckedHeapReadMemoryAdd(remaining_projection,
                                    &field_projection) ||
          !CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(bytes.capacity()),
              sizeof(idx::byte), &binary_bytes) ||
          !CheckedHeapReadMemoryAdd(
              control->retained_decode_row_memory_bytes,
              &field_phase_memory) ||
          !CheckedHeapReadMemoryAdd(sizeof(bytes), &field_phase_memory) ||
          !CheckedHeapReadMemoryAdd(binary_bytes, &field_phase_memory) ||
          !CheckedHeapReadMemoryAdd(field_projection,
                                    &field_phase_memory) ||
          !ObserveBoundedHeapReadMemory(control, field_phase_memory)) {
        if (control->refusal_detail.empty()) {
          control->refusal_detail =
              "heap_read_binary_field_memory_receipt_overflow";
        }
        return false;
      }
    }
    std::vector<std::string> field_order;
    field_order.reserve(column_count);
    for (std::uint32_t index = 0; index < column_count; ++index) {
      std::string field;
      if (!ReadBinaryString(bytes, &offset, &field) || field.empty()) {
        summary->malformed = true;
        summary->trusted = false;
        return false;
      }
      field_order.push_back(std::move(field));
    }
    std::vector<std::string> field_types;
    field_types.reserve(column_count);
    std::vector<std::uint8_t> native_field_type_tags;
    native_field_type_tags.reserve(column_count);
    if (version == kScopedRowBinaryNativePacketVersion) {
      for (std::uint32_t index = 0; index < column_count; ++index) {
        std::uint8_t tag = 0;
        if (!ReadBinaryU8(bytes, &offset, &tag) ||
            ScopedRowNativePacketTypeName(tag).empty()) {
          summary->malformed = true;
          summary->trusted = false;
          return false;
        }
        native_field_type_tags.push_back(tag);
        field_types.emplace_back(ScopedRowNativePacketTypeName(tag));
      }
    } else if (version >= kScopedRowBinaryGeneralVersion) {
      for (std::uint32_t index = 0; index < column_count; ++index) {
        std::string type_name;
        if (!ReadBinaryString(bytes, &offset, &type_name) ||
            type_name.empty()) {
          summary->malformed = true;
          summary->trusted = false;
          return false;
        }
        field_types.push_back(std::move(type_name));
      }
    } else {
      field_types.assign(column_count, "text");
    }
    std::uint64_t compact_first_event_sequence = 0;
    std::uint64_t compact_creator_tx = 0;
    EngineUuid compact_table_uuid;
    EngineUuid compact_temporary_session_uuid;
    std::uint8_t compact_flags = 0;
    const bool compact_batch = version >= kScopedRowBinaryVersion;
    if (compact_batch) {
      if (!ReadBinaryU64(bytes, &offset, &compact_first_event_sequence) ||
          !ReadBinaryU64(bytes, &offset, &compact_creator_tx) ||
          !ReadBinaryEngineUuid(bytes, &offset, &compact_table_uuid) ||
          !ReadBinaryEngineUuid(bytes, &offset, &compact_temporary_session_uuid, true) ||
          !ReadBinaryU8(bytes, &offset, &compact_flags) ||
          compact_table_uuid.is_nil() || compact_flags != 0) {
        summary->malformed = true;
        summary->trusted = false;
        return false;
      }
    }
    if (control != nullptr &&
        (control->decoded_row_versions > control->maximum_row_versions ||
         row_count > control->maximum_row_versions -
                         control->decoded_row_versions)) {
      control->refusal_detail = "heap_read_maximum_row_versions_exceeded";
      return false;
    }
    if (row_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()) ||
        static_cast<std::size_t>(row_count) >
            std::numeric_limits<std::size_t>::max() - rows->size()) {
      summary->malformed = true;
      summary->trusted = false;
      if (control != nullptr) {
        control->refusal_detail = "heap_read_row_reserve_overflow";
      }
      return false;
    }
    if (compact_batch && row_count != 0 &&
        row_count - 1 >
            std::numeric_limits<std::uint64_t>::max() -
                compact_first_event_sequence) {
      summary->malformed = true;
      summary->trusted = false;
      if (control != nullptr) {
        control->refusal_detail = "heap_read_event_sequence_overflow";
      }
      return false;
    }
    const std::size_t null_bitmap_bytes =
        (static_cast<std::size_t>(column_count) + 7u) / 8u;
    const std::size_t minimum_row_bytes =
        (compact_batch ? 32u : 45u) + null_bitmap_bytes;
    if (row_count >
        static_cast<std::uint64_t>((bytes.size() - offset) /
                                   minimum_row_bytes)) {
      summary->malformed = true;
      summary->trusted = false;
      if (control != nullptr) {
        control->refusal_detail = "heap_read_row_count_exceeds_segment";
      }
      return false;
    }
    const std::size_t batch_payload_start = offset;
    std::uint64_t batch_fixed_retained_projection = 0;
    std::uint64_t batch_retained_projection = 0;
    if (control != nullptr && control->maximum_memory_bytes != 0) {
      const auto field_order_memory =
          HeapReadStringVectorMemoryBytes(field_order);
      const auto field_type_memory =
          HeapReadStringVectorMemoryBytes(field_types);
      std::uint64_t binary_bytes = 0;
      std::uint64_t native_tag_bytes = 0;
      std::uint64_t target_row_structural_bytes = 0;
      std::uint64_t new_row_structural_bytes = 0;
      std::uint64_t value_pair_bytes = 0;
      std::uint64_t copied_key_bytes = 0;
      std::uint64_t source_payload_projection = 0;
      std::uint64_t compact_identity_projection = 0;
      std::uint64_t target_row_count =
          static_cast<std::uint64_t>(rows->size()) + row_count;
      std::uint64_t field_name_bytes = 0;
      for (const auto& field : field_order) {
        if (!AccountHeapReadOwnedString(field, &field_name_bytes)) {
          control->refusal_detail =
              "heap_read_binary_decode_memory_receipt_overflow";
          return false;
        }
      }
      if (!field_order_memory.has_value() ||
          !field_type_memory.has_value() ||
          !CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(bytes.capacity()),
              sizeof(idx::byte), &binary_bytes) ||
          !CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(
                  native_field_type_tags.capacity()),
              sizeof(std::uint8_t), &native_tag_bytes) ||
          !CheckedHeapReadMemoryMultiply(target_row_count,
                                         sizeof(CrudRowVersionRecord),
                                         &target_row_structural_bytes) ||
          !CheckedHeapReadMemoryMultiply(row_count,
                                         sizeof(CrudRowVersionRecord),
                                         &new_row_structural_bytes) ||
          !CheckedHeapReadMemoryMultiply(row_count, column_count,
                                         &value_pair_bytes) ||
          !CheckedHeapReadMemoryMultiply(
              value_pair_bytes,
              sizeof(std::pair<std::string, std::string>),
              &value_pair_bytes) ||
          !CheckedHeapReadMemoryMultiply(row_count, field_name_bytes,
                                         &copied_key_bytes) ||
          !CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(bytes.size() - offset), 4,
              &source_payload_projection)) {
        control->refusal_detail =
            "heap_read_binary_decode_memory_receipt_overflow";
        return false;
      }
      // All five identities are inline16 fields already included in the
      // row structural allocation; compact decode adds no UUID strings.
      batch_fixed_retained_projection = new_row_structural_bytes;
      if (!CheckedHeapReadMemoryAdd(value_pair_bytes,
                                    &batch_fixed_retained_projection) ||
          !CheckedHeapReadMemoryAdd(copied_key_bytes,
                                    &batch_fixed_retained_projection) ||
          !CheckedHeapReadMemoryAdd(compact_identity_projection,
                                    &batch_fixed_retained_projection)) {
        control->refusal_detail =
            "heap_read_binary_decode_memory_receipt_overflow";
        return false;
      }
      batch_retained_projection = batch_fixed_retained_projection;
      if (!CheckedHeapReadMemoryAdd(source_payload_projection,
                                    &batch_retained_projection)) {
        control->refusal_detail =
            "heap_read_binary_decode_memory_receipt_overflow";
        return false;
      }
      std::uint64_t phase_memory = control->retained_parent_memory_bytes;
      if (!CheckedHeapReadMemoryAdd(
              control->retained_decode_row_memory_bytes, &phase_memory) ||
          !CheckedHeapReadMemoryAdd(sizeof(bytes), &phase_memory) ||
          !CheckedHeapReadMemoryAdd(binary_bytes, &phase_memory) ||
          !CheckedHeapReadMemoryAdd(*field_order_memory, &phase_memory) ||
          !CheckedHeapReadMemoryAdd(*field_type_memory, &phase_memory) ||
          !CheckedHeapReadMemoryAdd(sizeof(native_field_type_tags),
                                    &phase_memory) ||
          !CheckedHeapReadMemoryAdd(native_tag_bytes, &phase_memory) ||
          !CheckedHeapReadMemoryAdd(sizeof(compact_table_uuid) +
                                      sizeof(compact_temporary_session_uuid),
                                    &phase_memory) ||
          !CheckedHeapReadMemoryAdd(target_row_structural_bytes,
                                    &phase_memory) ||
          !CheckedHeapReadMemoryAdd(batch_retained_projection,
                                    &phase_memory) ||
          !ObserveBoundedHeapReadMemory(control, phase_memory)) {
        if (control->refusal_detail.empty()) {
          control->refusal_detail =
              "heap_read_binary_decode_memory_receipt_overflow";
        }
        return false;
      }
    }
    rows->reserve(rows->size() + static_cast<std::size_t>(row_count));
    for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
      if (BoundedScopedReadCancelled(control)) { return false; }
      CrudRowVersionRecord row;
      std::uint8_t deleted = 0;
      if (compact_batch) {
        row.creator_tx = compact_creator_tx;
        row.event_sequence = compact_first_event_sequence + row_index;
        row.previous_sequence = 0;
        row.table_uuid = compact_table_uuid;
        row.temporary_session_uuid = compact_temporary_session_uuid;
        if (!ReadBinaryEngineUuid(bytes, &offset, &row.row_uuid) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.version_uuid) ||
            offset + null_bitmap_bytes > bytes.size()) {
          summary->malformed = true;
          summary->trusted = false;
          return false;
        }
      } else {
        if (!ReadBinaryU64(bytes, &offset, &row.creator_tx) ||
            !ReadBinaryU64(bytes, &offset, &row.event_sequence) ||
            !ReadBinaryU64(bytes, &offset, &row.previous_sequence) ||
            !ReadBinaryU8(bytes, &offset, &deleted) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.table_uuid) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.row_uuid) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.version_uuid) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.previous_version_uuid, true) ||
            !ReadBinaryEngineUuid(bytes, &offset, &row.temporary_session_uuid, true) ||
            offset + null_bitmap_bytes > bytes.size()) {
          summary->malformed = true;
          summary->trusted = false;
          return false;
        }
        if (deleted > 1 || (column_count == 0 && deleted == 0)) {
          summary->malformed = true; summary->trusted = false; return false;
        }
        row.deleted = deleted != 0;
      }
      row.sequence = row.event_sequence;
      if (compact_batch) {
        row.deleted = false;
      }
      const std::size_t null_bitmap_offset = offset;
      offset += null_bitmap_bytes;
      row.values.reserve(column_count);
      for (std::uint32_t column_index = 0; column_index < column_count;
           ++column_index) {
        const bool is_null =
            (bytes[null_bitmap_offset + column_index / 8u] &
             static_cast<idx::byte>(1u << (column_index % 8u))) != 0;
        if (is_null) {
          row.values.push_back({field_order[column_index], "<NULL>"});
          continue;
        }
        if (version == kScopedRowBinaryNativePacketVersion) {
          const std::uint8_t tag = native_field_type_tags[column_index];
          std::string_view payload;
          switch (tag) {
            case 3:
              if (offset + 1 > bytes.size()) {
                summary->malformed = true;
                summary->trusted = false;
                return false;
              }
              payload = std::string_view(
                  reinterpret_cast<const char*>(bytes.data() + offset), 1);
              offset += 1;
              break;
            case 4:
              if (offset + 4 > bytes.size()) {
                summary->malformed = true;
                summary->trusted = false;
                return false;
              }
              payload = std::string_view(
                  reinterpret_cast<const char*>(bytes.data() + offset), 4);
              offset += 4;
              break;
            case 2:
            case 5:
            case 6:
              if (offset + 8 > bytes.size()) {
                summary->malformed = true;
                summary->trusted = false;
                return false;
              }
              payload = std::string_view(
                  reinterpret_cast<const char*>(bytes.data() + offset), 8);
              offset += 8;
              break;
            case 1:
            case 7: {
              std::string value;
              if (!ReadBinaryString(bytes, &offset, &value) ||
              (field_types[column_index] == "uuid" && value.size() != 16)) {
                summary->malformed = true;
                summary->trusted = false;
                return false;
              }
              row.values.push_back(
                  {field_order[column_index],
                   ScopedRowBinaryMaterializeValue(field_types[column_index],
                                                   value)});
              continue;
            }
            default:
              summary->malformed = true;
              summary->trusted = false;
              return false;
          }
          row.values.push_back(
              {field_order[column_index],
               ScopedRowBinaryMaterializeValue(field_types[column_index],
                                               payload)});
        } else {
          std::string value;
          if (!ReadBinaryString(bytes, &offset, &value) ||
              (field_types[column_index] == "uuid" && value.size() != 16)) {
            summary->malformed = true;
            summary->trusted = false;
            return false;
          }
          row.values.push_back(
              {field_order[column_index],
               ScopedRowBinaryMaterializeValue(field_types[column_index],
                                               value)});
        }
      }
      ++summary->row_version_count;
      if (row.deleted) { ++summary->tombstone_count; }
      if (!row.previous_version_uuid.is_nil()) { ++summary->update_count; }
      if (!AdmitBoundedScopedRow(control, row)) { return false; }
      rows->push_back(std::move(row));
    }
    if (control != nullptr && control->maximum_memory_bytes != 0) {
      std::uint64_t retained_payload_projection = 0;
      std::uint64_t retained_batch_memory =
          batch_fixed_retained_projection;
      if (!CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(offset - batch_payload_start), 4,
              &retained_payload_projection) ||
          !CheckedHeapReadMemoryAdd(retained_payload_projection,
                                    &retained_batch_memory) ||
          !CheckedHeapReadMemoryAdd(
              retained_batch_memory,
              &control->retained_decode_row_memory_bytes)) {
        control->refusal_detail =
            "heap_read_binary_decode_memory_receipt_overflow";
        return false;
      }
    }
  }
  summary->trusted = true;
  published = true;
  return true;
}


void AppendTabField(std::string* line, std::string_view field) {
  if (line == nullptr) {
    return;
  }
  if (!line->empty()) {
    line->push_back('\t');
  }
  line->append(field.data(), field.size());
}

void AppendLineField(std::string* line, bool* first, std::string_view field) {
  if (line == nullptr || first == nullptr) { return; }
  if (!*first) {
    line->push_back('\t');
  }
  *first = false;
  line->append(field.data(), field.size());
}

void AppendLineU64Field(std::string* line, bool* first, std::uint64_t value) {
  if (line == nullptr || first == nullptr) { return; }
  char buffer[32];
  auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc()) { return; }
  AppendLineField(line, first, std::string_view(buffer, static_cast<std::size_t>(ptr - buffer)));
}

void AppendHexEncoded(std::string* out, std::string_view value) {
  if (out == nullptr) { return; }
  static constexpr char kHex[] = "0123456789abcdef";
  const std::size_t offset = out->size();
  out->resize(offset + value.size() * 2);
  char* cursor = out->data() + offset;
  for (const unsigned char c : value) {
    *cursor++ = kHex[(c >> 4) & 0x0f];
    *cursor++ = kHex[c & 0x0f];
  }
}

void AppendLineHexField(std::string* line,
                        bool* first,
                        std::string_view field) {
  if (line == nullptr || first == nullptr) { return; }
  if (!*first) {
    line->push_back('\t');
  }
  *first = false;
  line->append(kLineHexFieldPrefix.data(), kLineHexFieldPrefix.size());
  AppendHexEncoded(line, field);
}

bool LineFieldRequiresHex(std::string_view field) {
  if (field.rfind(kLineHexFieldPrefix, 0) == 0) {
    return true;
  }
  for (const char ch : field) {
    if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '\0') {
      return true;
    }
  }
  return false;
}

void AppendLineSafeOrHexField(std::string* line,
                              bool* first,
                              std::string_view field) {
  if (LineFieldRequiresHex(field)) {
    AppendLineHexField(line, first, field);
  } else {
    AppendLineField(line, first, field);
  }
}

std::size_t EncodedCrudPairsSize(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::size_t size = 0;
  for (const auto& [key, value] : pairs) {
    if (size != 0) { ++size; }
    size += key.size() * 2 + 1 + value.size() * 2;
  }
  return size;
}

void AppendEncodedCrudPairsField(
    std::string* line,
    bool* first,
    const std::vector<std::pair<std::string, std::string>>& pairs) {
  if (line == nullptr || first == nullptr) { return; }
  if (!*first) {
    line->push_back('\t');
  }
  *first = false;
  bool first_pair = true;
  for (const auto& [key, value] : pairs) {
    if (!first_pair) {
      line->push_back('|');
    }
    first_pair = false;
    AppendHexEncoded(line, key);
    line->push_back('=');
    AppendHexEncoded(line, value);
  }
}

void AppendEncodedCrudPairsFieldWithEncodedKeys(
    std::string* line,
    bool* first,
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const std::vector<std::string>* encoded_keys) {
  if (line == nullptr || first == nullptr) { return; }
  if (encoded_keys == nullptr || encoded_keys->size() != pairs.size()) {
    AppendEncodedCrudPairsField(line, first, pairs);
    return;
  }
  if (!*first) {
    line->push_back('\t');
  }
  *first = false;
  for (std::size_t index = 0; index < pairs.size(); ++index) {
    if (index != 0) {
      line->push_back('|');
    }
    line->append((*encoded_keys)[index]);
    line->push_back('=');
    AppendHexEncoded(line, pairs[index].second);
  }
}

void AppendEncodedTypedFieldsFieldWithEncodedKeys(
    std::string* line,
    bool* first,
    const EngineRowValue& row,
    std::span<const std::string> field_order,
    const std::vector<std::string>& encoded_keys) {
  if (line == nullptr || first == nullptr ||
      field_order.size() != row.fields.size() ||
      encoded_keys.size() != field_order.size()) {
    return;
  }
  if (!*first) {
    line->push_back('\t');
  }
  *first = false;
  for (std::size_t index = 0; index < row.fields.size(); ++index) {
    if (index != 0) {
      line->push_back('|');
    }
    line->append(encoded_keys[index]);
    line->push_back('=');
    const auto& typed = row.fields[index].second;
    AppendHexEncoded(line, typed.isSqlNull() ? std::string_view("<NULL>")
                                             : std::string_view(typed.encoded_value));
  }
}

void ReserveAmortizedAppendCapacity(std::string* out, std::size_t extra) {
  if (out == nullptr) { return; }
  const std::size_t required = out->size() + extra;
  if (required <= out->capacity()) { return; }
  std::size_t grown = out->capacity() == 0 ? 4096 : out->capacity();
  while (grown < required) {
    const std::size_t next = grown * 2;
    if (next <= grown) {
      grown = required;
      break;
    }
    grown = next;
  }
  out->reserve(grown);
}

void AppendRowVersionStoreLine(std::string* out, const CrudRowVersionRecord& row,
    std::uint64_t event_sequence_override,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::vector<std::string>* encoded_keys) {
  if (!out) throw std::invalid_argument("row output required");
  (void)encoded_keys;
  EngineRowValue typed;
  std::vector<std::string> order;
  for (const auto& [key, value] : values) {
    EngineTypedValue field;
    field.descriptor.canonical_type_name = "text";
    field.encoded_value = value;
    if (value == "<NULL>") field.setState(EngineValueState::sql_null);
    typed.fields.emplace_back(key, std::move(field));
    order.push_back(key);
  }
  std::string encoded;
  if (!AppendScopedRowBinaryBatch(&encoded, {row}, std::span<const EngineRowValue>(&typed,1),
                                 order, event_sequence_override))
    throw std::invalid_argument("invalid native row record");
  out->append(encoded);
}
void AppendTypedRowVersionStoreLine(std::string* out, const CrudRowVersionRecord& row,
    std::uint64_t event_sequence_override, const EngineRowValue& typed_row,
    std::span<const std::string> field_order, const std::vector<std::string>& encoded_keys) {
  if (!out) throw std::invalid_argument("row output required");
  (void)encoded_keys;
  std::string encoded;
  if (!AppendScopedRowBinaryBatch(&encoded, {row}, std::span<const EngineRowValue>(&typed_row,1),
                                 field_order, event_sequence_override))
    throw std::invalid_argument("invalid native typed row record");
  out->append(encoded);
}

void AppendRowVersionStoreLine(std::string* out,
                               const CrudRowVersionRecord& row,
                               std::uint64_t event_sequence_override) {
  AppendRowVersionStoreLine(out, row, event_sequence_override, row.values);
}

void AppendRowVersionStoreLine(std::string* out,
                               const CrudRowVersionRecord& row) {
  AppendRowVersionStoreLine(out, row, row.event_sequence);
}

std::string BuildRowVersionStoreLine(const CrudRowVersionRecord& row) {
  std::string line;
  AppendRowVersionStoreLine(&line, row);
  return line;
}


}  // namespace scratchbird::engine::internal_api
