// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_locator.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "hash_digest.hpp"
#include "dml/test_optimization_profile.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_ROW_VERSION_READER_IMPLEMENTATION_AUTHORITY
// Owns physical row-version segment decoding and cache validation. Visibility
// and finality remain projections of the durable MGA transaction inventory.

constexpr const char* kRowStoreMagic = "SBMGA1";

std::string ScopedRelationStoreRoot(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_scope";
}

std::string ScopedRowStorePath(const EngineRequestContext& context,
    const EngineUuid& table_uuid) {
  return MgaScopedRelationPath(context, table_uuid, ".rows", false);
}

std::string ScopedRowBinaryStorePath(const EngineRequestContext& context,
    const EngineUuid& table_uuid) {
  return MgaScopedRelationPath(context, table_uuid, ".rows.sbnr", false);
}

bool InspectRowSegment(const std::string& path, bool* present) {
  *present = false;
  std::error_code error;
  const auto link = std::filesystem::symlink_status(path, error);
  if (link.type() == std::filesystem::file_type::not_found &&
      (!error || error == std::errc::no_such_file_or_directory)) return true;
  if (error) return false;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) return false;
  *present = true;
  return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto tab = line.find('\t', start);
    fields.push_back(line.substr(start, tab == std::string::npos
                                           ? std::string::npos
                                           : tab - start));
    if (tab == std::string::npos) { break; }
    start = tab + 1;
  }
  return fields;
}

std::uint64_t ParseU64(const std::string& text) {
  std::uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} ? value : 0;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + (c - 'a'); }
  if (c >= 'A' && c <= 'F') { return 10 + (c - 'A'); }
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  std::string decoded;
  if ((encoded.size() % 2) != 0) { return decoded; }
  decoded.reserve(encoded.size() / 2);
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    const int hi = HexValue(encoded[i]);
    const int lo = HexValue(encoded[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    decoded.push_back(static_cast<char>((hi << 4) | lo));
  }
  return decoded;
}

std::vector<std::pair<std::string, std::string>> DecodeCrudPairsWithKeyCache(
    const std::string& encoded,
    std::unordered_map<std::string, std::string>* decoded_key_cache) {
  std::vector<std::pair<std::string, std::string>> pairs;
  pairs.reserve(8);
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto pipe = encoded.find('|', start);
    const std::size_t end = pipe == std::string::npos ? encoded.size() : pipe;
    const auto equals = encoded.find('=', start);
    if (equals != std::string::npos && equals < end) {
      std::string key;
      const std::string encoded_key = encoded.substr(start, equals - start);
      if (decoded_key_cache != nullptr) {
        auto found = decoded_key_cache->find(encoded_key);
        if (found == decoded_key_cache->end()) {
          auto inserted =
              decoded_key_cache->emplace(encoded_key, DecodeCrudTextLocal(encoded_key));
          found = inserted.first;
        }
        key = found->second;
      } else {
        key = DecodeCrudTextLocal(encoded_key);
      }
      pairs.push_back({std::move(key),
                       DecodeCrudTextLocal(encoded.substr(equals + 1,
                                                          end - equals - 1))});
    }
    if (pipe == std::string::npos) { break; }
    start = pipe + 1;
  }
  return pairs;
}

struct ScopedDecodedRowCacheEntry {
  std::vector<CrudRowVersionRecord> rows;
  scratchbird::core::hash::Digest256 text_digest{};
  scratchbird::core::hash::Digest256 binary_digest{};
};

std::mutex& ScopedDecodedRowCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, ScopedDecodedRowCacheEntry>&
ScopedDecodedRowCache() {
  static std::unordered_map<std::string, ScopedDecodedRowCacheEntry> cache;
  return cache;
}

}  // namespace

void UpdateScopedDecodedRowCacheAfterAppend(
    const std::map<std::string, std::vector<CrudRowVersionRecord>>&
        decoded_appends_by_path,
    const std::map<std::string, std::string>& encoded_appends_by_path) {
  // Append completion invalidates the old content receipt. Writer hints alone
  // cannot prove the full readable file; the next reader admits and decodes it
  // once before subsequent readers may reuse the digest-bound decoded rows.
  const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
  auto& cache = ScopedDecodedRowCache();
  for (const auto& [path, rows] : decoded_appends_by_path) cache.erase(path);
  for (const auto& [path, bytes] : encoded_appends_by_path) cache.erase(path);
}

bool LoadDecodedScopedRowsForTable(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment) {
  if (rows == nullptr || !core::uuid::IsEngineIdentityUuid(table_uuid)) return false;
  rows->clear();
  if (used_segment != nullptr) *used_segment = false;
  const std::string path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_path = ScopedRowBinaryStorePath(context, table_uuid);
  std::vector<scratchbird::core::index::byte> text_bytes, binary_bytes;
  const auto invalidate = [&] {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    ScopedDecodedRowCache().erase(path);
    return false;
  };
  if (!ReadCompleteMgaBinaryFile(path, &text_bytes) ||
      !ReadCompleteMgaBinaryFile(binary_path, &binary_bytes)) return invalidate();
  // Cache reuse never substitutes for current complete read admission. Bind
  // both exact contents, not a summed size or timestamps which can be reused.
  const auto text_digest = scratchbird::core::hash::ComputeSha256Digest(text_bytes);
  const auto binary_digest = scratchbird::core::hash::ComputeSha256Digest(binary_bytes);
  if (!text_digest.ok() || !binary_digest.ok()) return invalidate();
  const bool any_segment = !text_bytes.empty() || !binary_bytes.empty();
  if (!dml::TestScanScalarProfile()) {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    const auto cached = ScopedDecodedRowCache().find(path);
    if (cached != ScopedDecodedRowCache().end()) {
      if (cached->second.text_digest == text_digest.digest &&
          cached->second.binary_digest == binary_digest.digest) {
        *rows = cached->second.rows;
        if (used_segment != nullptr) *used_segment = any_segment;
        dml::RecordTestOptimizationBranch("decoded_row_cache_hit");
        return true;
      }
      ScopedDecodedRowCache().erase(cached);
    }
  }
  dml::RecordTestOptimizationBranch("decoded_rows_from_store");
  std::vector<CrudRowVersionRecord> decoded_rows;
  ScopedRelationSummary general_summary;
  if (!DecodeScopedRowBinaryBytes(text_bytes, &decoded_rows, &general_summary) ||
      general_summary.malformed) return invalidate();
  ScopedRelationSummary binary_summary;
  if (!DecodeScopedRowBinaryBytes(binary_bytes, &decoded_rows, &binary_summary) ||
      binary_summary.malformed) return invalidate();
  if (std::any_of(decoded_rows.begin(), decoded_rows.end(),
          [&](const auto& row) { return row.table_uuid != table_uuid; })) return invalidate();
  if (!dml::TestScanScalarProfile()) {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    ScopedDecodedRowCacheEntry entry;
    entry.rows = decoded_rows;
    entry.text_digest = text_digest.digest;
    entry.binary_digest = binary_digest.digest;
    ScopedDecodedRowCache()[path] = std::move(entry);
  }
  *rows = std::move(decoded_rows);
  if (used_segment != nullptr) *used_segment = any_segment;
  return true;
}

bool LoadDecodedScopedRowsForTableBounded(
    const EngineRequestContext& context,
    const EngineUuid& table_uuid,
    BoundedScopedRowReadControl* control,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment) {
  if (control == nullptr || rows == nullptr ||
      !core::uuid::IsEngineIdentityUuid(table_uuid) ||
      control->maximum_row_versions == 0 || control->maximum_bytes == 0 ||
      control->cancellation_requested == nullptr ||
      !*control->cancellation_requested) {
    return false;
  }
  rows->clear();
  if (used_segment != nullptr) { *used_segment = false; }
  if (BoundedScopedReadCancelled(control)) { return false; }

  std::uint64_t path_projection = control->retained_parent_memory_bytes;
  std::uint64_t path_character_bytes = 128;
  std::uint64_t path_dynamic_bytes = 0;
  if (!CheckedHeapReadMemoryAdd(
          static_cast<std::uint64_t>(context.database_path.size()),
          &path_character_bytes) ||
      !CheckedHeapReadMemoryAdd(std::uint64_t{36},
                                &path_character_bytes) ||
      !CheckedHeapReadMemoryMultiply(path_character_bytes, 2,
                                     &path_dynamic_bytes) ||
      !CheckedHeapReadMemoryAdd(path_dynamic_bytes, &path_projection) ||
      !ObserveBoundedHeapReadMemory(control, path_projection)) {
    if (control->refusal_detail.empty()) {
      control->refusal_detail =
          "heap_read_path_memory_receipt_overflow";
    }
    return false;
  }
  const std::string text_path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_path =
      ScopedRowBinaryStorePath(context, table_uuid);
  const auto text_existence_started = std::chrono::steady_clock::now();
  bool text_exists = false;
  const bool text_admitted = InspectRowSegment(text_path, &text_exists);
  if (!AccountHeapReadWait(control, text_existence_started)) return false;
  const auto binary_existence_started = std::chrono::steady_clock::now();
  bool binary_exists = false;
  const bool binary_admitted = InspectRowSegment(binary_path, &binary_exists);
  if (!AccountHeapReadWait(control, binary_existence_started)) return false;
  if (!text_admitted || !binary_admitted) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
    control->refusal_detail = "heap_read_scoped_segment_status_failed";
    return false;
  }
  if (!text_exists && !binary_exists) { return true; }

  const auto authorize_file = [&](const std::string& path,
                                  std::uint64_t* authorized_file_bytes) {
    if (authorized_file_bytes == nullptr) { return false; }
    std::error_code ignored;
    const auto size_started = std::chrono::steady_clock::now();
    const auto size = std::filesystem::file_size(path, ignored);
    if (!AccountHeapReadWait(control, size_started)) return false;
    if (ignored || size == static_cast<std::uintmax_t>(-1) ||
        size > std::numeric_limits<std::uint64_t>::max()) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_scoped_segment_size_unavailable";
      return false;
    }
    *authorized_file_bytes = static_cast<std::uint64_t>(size);
    return true;
  };
  std::uint64_t authorized_text_bytes = 0;
  std::uint64_t authorized_binary_bytes = 0;
  if ((text_exists &&
       !authorize_file(text_path, &authorized_text_bytes)) ||
      (binary_exists &&
       !authorize_file(binary_path, &authorized_binary_bytes))) {
    return false;
  }

  std::vector<CrudRowVersionRecord> decoded_rows;
  std::unordered_map<std::string, std::string> row_value_key_cache;
  std::uint64_t initial_decode_memory = control->retained_parent_memory_bytes;
  std::uint64_t initial_allocation_bytes = 0;
  constexpr std::uint64_t kInitialDecodedKeyCacheEntries = 64;
  if (!AccountHeapReadOwnedString(text_path, &initial_decode_memory) ||
      !AccountHeapReadOwnedString(binary_path, &initial_decode_memory) ||
      !CheckedHeapReadMemoryMultiply(kInitialDecodedKeyCacheEntries,
                                     2 * sizeof(void*),
                                     &initial_allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(initial_allocation_bytes,
                                &initial_decode_memory) ||
      !CheckedHeapReadMemoryMultiply(
          kInitialDecodedKeyCacheEntries,
          sizeof(std::pair<const std::string, std::string>) +
              4 * sizeof(void*),
          &initial_allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(initial_allocation_bytes,
                                &initial_decode_memory) ||
      !ObserveBoundedHeapReadMemory(control, initial_decode_memory)) {
    if (control->refusal_detail.empty()) {
      control->refusal_detail =
          "heap_read_decode_initial_memory_receipt_overflow";
    }
    return false;
  }
  row_value_key_cache.reserve(kInitialDecodedKeyCacheEntries);
  const auto decode_parent_memory = [&]() -> std::optional<std::uint64_t> {
    std::uint64_t bytes = control->retained_parent_memory_bytes;
    const auto cache_memory =
        HeapReadStringCacheMemoryBytes(row_value_key_cache);
    if (!cache_memory.has_value() ||
        !AccountHeapReadOwnedString(text_path, &bytes) ||
        !AccountHeapReadOwnedString(binary_path, &bytes) ||
        !CheckedHeapReadMemoryAdd(*cache_memory, &bytes)) {
      return std::nullopt;
    }
    return bytes;
  };
  if (text_exists) {
    const auto parent_memory = decode_parent_memory();
    const auto retained_row_memory = HeapReadRowVectorMemoryBytes(decoded_rows);
    if (!parent_memory || !retained_row_memory) {
      control->refusal_detail = "heap_read_general_parent_memory_overflow";
      return false;
    }
    control->retained_parent_memory_bytes = *parent_memory;
    control->retained_decode_row_memory_bytes = *retained_row_memory;
    ScopedRelationSummary summary;
    if (!DecodeScopedRowBinaryStore(text_path, &decoded_rows, &summary, control, authorized_text_bytes) || summary.malformed) {
      if (summary.malformed) control->failure_category = MgaHeapReadFailureCategoryV1::kCorruptStorage;
      if (control->refusal_detail.empty()) control->refusal_detail = "heap_read_general_binary_decode_failed";
      return false;
    }
  }
  if (binary_exists) {
    const auto parent_memory = decode_parent_memory();
    const auto retained_row_memory =
        HeapReadRowVectorMemoryBytes(decoded_rows);
    if (!parent_memory.has_value() || !retained_row_memory.has_value()) {
      control->refusal_detail = "heap_read_binary_parent_memory_overflow";
      return false;
    }
    control->retained_parent_memory_bytes = *parent_memory;
    control->retained_decode_row_memory_bytes = *retained_row_memory;
    ScopedRelationSummary binary_summary;
    if (!DecodeScopedRowBinaryStore(binary_path,
                                    &decoded_rows,
                                    &binary_summary,
                                    control,
                                    authorized_binary_bytes) ||
        binary_summary.malformed) {
      if (binary_summary.malformed) {
        control->failure_category =
            MgaHeapReadFailureCategoryV1::kCorruptStorage;
      }
      if (control->refusal_detail.empty()) {
        control->failure_category =
            MgaHeapReadFailureCategoryV1::kCorruptStorage;
        control->refusal_detail = "heap_read_scoped_binary_decode_failed";
      }
      return false;
    }
  }
  if (std::any_of(decoded_rows.begin(), decoded_rows.end(),
          [&](const auto& row) { return row.table_uuid != table_uuid; })) {
    control->failure_category = MgaHeapReadFailureCategoryV1::kCorruptStorage;
    control->refusal_detail = "heap_read_scoped_binary_table_mismatch";
    return false;
  }
  *rows = std::move(decoded_rows);
  if (used_segment != nullptr) *used_segment = authorized_text_bytes != 0 || authorized_binary_bytes != 0;
  return true;
}

void ClearScopedDecodedRowCache() {
  const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
  ScopedDecodedRowCache().clear();
}

}  // namespace scratchbird::engine::internal_api
