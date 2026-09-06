// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "secondary_index_delta_merge.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr std::string_view kScopedRowBinaryBatchMagic = "SBMRBIN1";
inline constexpr std::uint16_t kScopedRowBinaryVersion = 3;
inline constexpr std::uint16_t kScopedRowBinaryLegacyTypedVersion = 2;
inline constexpr std::uint16_t kScopedRowBinaryNativePacketVersion = 4;
inline constexpr std::string_view kScopedIndexBinaryBatchMagic = "SBMIBIN1";
inline constexpr std::uint16_t kScopedIndexBinaryVersion = 1;

// SEARCH_KEY: SB_ENGINE_MGA_ROW_CODEC_INTERFACE
// Row framing converts persisted bytes/text to version records and back. It
// validates format/resource bounds but never decides MGA visibility/finality.
struct ScopedRelationSummary {
  bool trusted = false;
  bool malformed = false;
  std::uint64_t row_version_count = 0;
  std::uint64_t tombstone_count = 0;
  std::uint64_t update_count = 0;
};

struct BoundedScopedRowReadControl {
  std::uint64_t maximum_row_versions = 0;
  std::uint64_t maximum_bytes = 0;
  std::uint64_t maximum_memory_bytes = 0;
  std::uint64_t retained_parent_memory_bytes = 0;
  std::uint64_t retained_decode_row_memory_bytes = 0;
  std::uint64_t peak_live_memory_bytes = 0;
  const std::function<bool()>* cancellation_requested = nullptr;
  std::uint64_t decoded_row_versions = 0;
  std::uint64_t decoded_bytes = 0;
  bool cancellation_observed = false;
  MgaHeapReadFailureCategoryV1 failure_category =
      MgaHeapReadFailureCategoryV1::kNone;
  std::string refusal_detail;
  HeapReadRuntimeObservation* runtime_observation = nullptr;
};

std::vector<scratchbird::core::index::byte> ReadBinaryFile(
    const std::string& path);
void AppendBinaryU8(std::string* out, std::uint8_t value);
void AppendBinaryU16(std::string* out, std::uint16_t value);
void AppendBinaryU32(std::string* out, std::uint32_t value);
void AppendBinaryU64(std::string* out, std::uint64_t value);
bool AppendBinaryString(std::string* out, std::string_view value);
bool AppendBinaryUuidText(std::string* out, const std::string& text);
std::string ScopedRowBinaryMaterializeValue(std::string_view type_name,
                                            std::string_view payload);
std::string_view ScopedRowNativePacketTypeName(std::uint8_t tag);
bool ReadBinaryU8(const std::vector<scratchbird::core::index::byte>& bytes,
                  std::size_t* offset,
                  std::uint8_t* out);
bool ReadBinaryU16(const std::vector<scratchbird::core::index::byte>& bytes,
                   std::size_t* offset,
                   std::uint16_t* out);
bool ReadBinaryU32(const std::vector<scratchbird::core::index::byte>& bytes,
                   std::size_t* offset,
                   std::uint32_t* out);
bool ReadBinaryU64(const std::vector<scratchbird::core::index::byte>& bytes,
                   std::size_t* offset,
                   std::uint64_t* out);
bool ReadBinaryString(
    const std::vector<scratchbird::core::index::byte>& bytes,
    std::size_t* offset,
    std::string* out);
bool ReadBinaryUuidText(
    const std::vector<scratchbird::core::index::byte>& bytes,
    std::size_t* offset,
    std::string* out);
bool AppendScopedRowBinaryBatch(
    std::string* out,
    const std::vector<CrudRowVersionRecord>& rows,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    std::uint64_t first_event_sequence);
bool AppendScopedRowIdentityBinaryBatch(
    std::string* out,
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence);
bool AppendScopedRowIdentityNativePacketBatch(
    std::string* out,
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    const EngineNativeRowPacketFrame& frame,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence);
bool CheckedHeapReadMemoryAdd(std::uint64_t value, std::uint64_t* total);
bool CheckedHeapReadMemoryMultiply(std::uint64_t left,
                                   std::uint64_t right,
                                   std::uint64_t* product);
bool AccountHeapReadOwnedString(const std::string& value,
                                std::uint64_t* total);
std::optional<std::uint64_t> HeapReadRowVectorMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
bool AccountHeapReadRowDynamicMemoryBytes(
    const CrudRowVersionRecord& row,
    std::uint64_t* total);
std::optional<std::uint64_t> HeapReadVersionIndexProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
std::optional<std::uint64_t> HeapReadVisibilityMapProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
std::optional<std::uint64_t> HeapReadStringCacheMemoryBytes(
    const std::unordered_map<std::string, std::string>& cache);
bool ObserveBoundedHeapReadMemory(BoundedScopedRowReadControl* control,
                                  std::uint64_t live_bytes);
bool AccountHeapReadWait(
    BoundedScopedRowReadControl* control,
    std::chrono::steady_clock::time_point started);
bool AccountHeapStorageBytes(BoundedScopedRowReadControl* control,
                             std::uint64_t bytes);
bool BoundedScopedReadCancelled(BoundedScopedRowReadControl* control);
bool AdmitBoundedScopedRow(BoundedScopedRowReadControl* control,
                           const CrudRowVersionRecord& row);
bool DecodeScopedRowBinaryStore(
    const std::string& path,
    std::vector<CrudRowVersionRecord>* rows,
    ScopedRelationSummary* summary,
    BoundedScopedRowReadControl* control = nullptr,
    std::uint64_t authorized_file_bytes = 0);

void AppendLineField(std::string* line,
                     bool* first,
                     std::string_view field);
void AppendLineU64Field(std::string* line,
                        bool* first,
                        std::uint64_t value);
void AppendLineSafeOrHexField(std::string* line,
                              bool* first,
                              std::string_view field);
void AppendHexEncoded(std::string* out, std::string_view value);
void ReserveAmortizedAppendCapacity(std::string* out, std::size_t extra);
void AppendRowVersionStoreLine(
    std::string* out,
    const CrudRowVersionRecord& row,
    std::uint64_t event_sequence_override,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::vector<std::string>* encoded_keys = nullptr);
void AppendTypedRowVersionStoreLine(
    std::string* out,
    const CrudRowVersionRecord& row,
    std::uint64_t event_sequence_override,
    const EngineRowValue& typed_row,
    std::span<const std::string> field_order,
    const std::vector<std::string>& encoded_keys);
void AppendRowVersionStoreLine(std::string* out,
                               const CrudRowVersionRecord& row,
                               std::uint64_t event_sequence_override);
void AppendRowVersionStoreLine(std::string* out,
                               const CrudRowVersionRecord& row);
std::string BuildRowVersionStoreLine(const CrudRowVersionRecord& row);

}  // namespace scratchbird::engine::internal_api
