// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_row_version_reader.hpp"

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

std::string ScopedRelationSegmentName(const std::string& table_uuid) {
  std::string name;
  name.reserve(table_uuid.size());
  for (const char ch : table_uuid) {
    const bool safe = (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') ||
                      ch == '-' || ch == '_';
    name.push_back(safe ? ch : '_');
  }
  return name.empty() ? std::string("unknown") : name;
}

std::string ScopedRowStorePath(const EngineRequestContext& context,
                               const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows";
}

std::string ScopedRowBinaryStorePath(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".rows.sbnr";
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(std::move(line));
  }
  return lines;
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
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  std::vector<CrudRowVersionRecord> rows;
};

struct ScopedRelationFileIdentity {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  bool ok = false;
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

ScopedRelationFileIdentity ScopedRelationTextFileIdentity(
    const std::string& path) {
  ScopedRelationFileIdentity identity;
  std::error_code ignored;
  const auto file_size = std::filesystem::file_size(path, ignored);
  if (ignored || file_size == static_cast<std::uintmax_t>(-1)) {
    return identity;
  }
  ignored.clear();
  const auto mtime = std::filesystem::last_write_time(path, ignored);
  if (ignored) {
    return identity;
  }
  identity.file_size = file_size;
  identity.file_mtime_ticks =
      static_cast<std::int64_t>(mtime.time_since_epoch().count());
  identity.ok = true;
  return identity;
}

ScopedRelationFileIdentity ExistingFileIdentity(const std::string& path) {
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return {};
  }
  return ScopedRelationTextFileIdentity(path);
}

}  // namespace

void UpdateScopedDecodedRowCacheAfterAppend(
    const std::map<std::string, std::vector<CrudRowVersionRecord>>&
        decoded_appends_by_path,
    const std::map<std::string, std::string>& encoded_appends_by_path) {
  if (decoded_appends_by_path.empty()) { return; }
  const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
  auto& cache = ScopedDecodedRowCache();
  for (const auto& [path, decoded_rows] : decoded_appends_by_path) {
    if (decoded_rows.empty()) { continue; }
    if (decoded_rows.size() > kScopedDecodedRowCacheMaxAutoWarmRows) {
      cache.erase(path);
      continue;
    }
    const auto encoded = encoded_appends_by_path.find(path);
    if (encoded == encoded_appends_by_path.end()) {
      cache.erase(path);
      continue;
    }
    const auto identity = ScopedRelationTextFileIdentity(path);
    if (!identity.ok) {
      cache.erase(path);
      continue;
    }
    const std::uintmax_t appended_bytes =
        static_cast<std::uintmax_t>(encoded->second.size());
    auto existing = cache.find(path);
    if (existing == cache.end()) {
      if (identity.file_size == appended_bytes) {
        cache.emplace(path,
                      ScopedDecodedRowCacheEntry{identity.file_size,
                                                 identity.file_mtime_ticks,
                                                 decoded_rows});
      }
      continue;
    }
    if (existing->second.rows.size() + decoded_rows.size() >
        kScopedDecodedRowCacheMaxAutoWarmRows) {
      cache.erase(existing);
      continue;
    }
    if (identity.file_size < appended_bytes ||
        existing->second.file_size != identity.file_size - appended_bytes) {
      cache.erase(existing);
      if (identity.file_size == appended_bytes) {
        cache.emplace(path,
                      ScopedDecodedRowCacheEntry{identity.file_size,
                                                 identity.file_mtime_ticks,
                                                 decoded_rows});
      }
      continue;
    }
    existing->second.rows.insert(existing->second.rows.end(),
                                 decoded_rows.begin(),
                                 decoded_rows.end());
    existing->second.file_size = identity.file_size;
    existing->second.file_mtime_ticks = identity.file_mtime_ticks;
  }
}

bool LoadDecodedScopedRowsForTable(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment) {
  if (rows == nullptr) { return false; }
  rows->clear();
  if (used_segment != nullptr) { *used_segment = false; }
  const std::string path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_path = ScopedRowBinaryStorePath(context, table_uuid);
  const bool text_exists = FileExistsAndNotEmpty(path);
  const bool binary_exists = FileExistsAndNotEmpty(binary_path);
  if (!text_exists && !binary_exists) {
    return true;
  }
  if (used_segment != nullptr) { *used_segment = true; }
  std::error_code ignored;
  std::uintmax_t file_size = 0;
  if (text_exists) {
    const auto identity = ScopedRelationTextFileIdentity(path);
    if (!identity.ok) {
      return false;
    }
    file_size += identity.file_size;
  }
  if (binary_exists) {
    ignored.clear();
    const auto binary_size = std::filesystem::file_size(binary_path, ignored);
    if (ignored || binary_size == static_cast<std::uintmax_t>(-1)) {
      return false;
    }
    file_size += binary_size;
  }
  if (file_size == 0) {
    return false;
  }
  {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    const auto cached = ScopedDecodedRowCache().find(path);
    if (cached != ScopedDecodedRowCache().end()) {
      const auto identity = text_exists
                                ? ScopedRelationTextFileIdentity(path)
                                : ScopedRelationFileIdentity{};
      const bool text_identity_matches =
          !text_exists ||
          (identity.ok && cached->second.file_mtime_ticks ==
                              identity.file_mtime_ticks);
      if (cached->second.file_size == file_size && text_identity_matches) {
        *rows = cached->second.rows;
        return true;
      }
      ScopedDecodedRowCache().erase(cached);
    }
  }

  std::vector<CrudRowVersionRecord> decoded_rows;
  std::unordered_map<std::string, std::string> row_value_key_cache;
  row_value_key_cache.reserve(64);
  if (text_exists) {
    for (const auto& line : ReadLines(path)) {
      const auto fields = SplitTabs(line);
      if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
          fields[1] != "ROW_VERSION") {
        continue;
      }
      CrudRowVersionRecord row;
      row.creator_tx = ParseU64(fields[2]);
      row.event_sequence = ParseU64(fields[3]);
      row.sequence = row.event_sequence;
      row.table_uuid = fields[4];
      row.row_uuid = fields[5];
      row.version_uuid = fields[6];
      row.deleted = fields[7] == "1";
      row.previous_version_uuid = fields[8];
      row.previous_sequence = ParseU64(fields[9]);
      row.values = DecodeCrudPairsWithKeyCache(fields[10], &row_value_key_cache);
      if (fields.size() >= 12) {
        row.temporary_session_uuid = fields[11];
      }
      decoded_rows.push_back(std::move(row));
    }
  }
  if (binary_exists) {
    ScopedRelationSummary binary_summary;
    if (!DecodeScopedRowBinaryStore(binary_path,
                                    &decoded_rows,
                                    &binary_summary) ||
        binary_summary.malformed) {
      return false;
    }
  }
  {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    const auto identity = text_exists
                              ? ScopedRelationTextFileIdentity(path)
                              : ScopedRelationFileIdentity{};
    ScopedDecodedRowCache()[path] = {
        file_size,
        identity.ok ? identity.file_mtime_ticks : 0,
        decoded_rows};
  }
  *rows = std::move(decoded_rows);
  return true;
}

bool LoadDecodedScopedRowsForTableBounded(
    const EngineRequestContext& context,
    const std::string& table_uuid,
    BoundedScopedRowReadControl* control,
    std::vector<CrudRowVersionRecord>* rows,
    bool* used_segment) {
  if (control == nullptr || rows == nullptr ||
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
      !CheckedHeapReadMemoryAdd(static_cast<std::uint64_t>(table_uuid.size()),
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
  const bool text_exists = FileExistsAndNotEmpty(text_path);
  if (!AccountHeapReadWait(control, text_existence_started)) return false;
  const auto binary_existence_started = std::chrono::steady_clock::now();
  const bool binary_exists = FileExistsAndNotEmpty(binary_path);
  if (!AccountHeapReadWait(control, binary_existence_started)) return false;
  if (!text_exists && !binary_exists) { return true; }
  if (used_segment != nullptr) { *used_segment = true; }

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
    constexpr std::uint64_t kMinimumTextRowRecordBytes = 27;
    const std::uint64_t maximum_text_rows = std::min(
        control->maximum_row_versions,
        (authorized_text_bytes + kMinimumTextRowRecordBytes - 1) /
            kMinimumTextRowRecordBytes);
    std::uint64_t text_vector_bytes = 0;
    std::uint64_t text_preflight_memory = initial_decode_memory;
    if (!CheckedHeapReadMemoryMultiply(maximum_text_rows,
                                       sizeof(CrudRowVersionRecord),
                                       &text_vector_bytes) ||
        !CheckedHeapReadMemoryAdd(sizeof(decoded_rows),
                                  &text_preflight_memory) ||
        !CheckedHeapReadMemoryAdd(text_vector_bytes,
                                  &text_preflight_memory) ||
        !CheckedHeapReadMemoryAdd(authorized_text_bytes,
                                  &text_preflight_memory) ||
        !ObserveBoundedHeapReadMemory(control, text_preflight_memory)) {
      if (control->refusal_detail.empty()) {
        control->refusal_detail =
            "heap_read_text_memory_receipt_overflow";
      }
      return false;
    }
    decoded_rows.reserve(static_cast<std::size_t>(maximum_text_rows));
    std::uint64_t retained_text_row_projection =
        sizeof(decoded_rows) + text_vector_bytes;
    std::ifstream input;
    const auto open_started = std::chrono::steady_clock::now();
    input.open(text_path, std::ios::binary);
    if (!AccountHeapReadWait(control, open_started)) return false;
    if (!input) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_scoped_text_open_failed";
      return false;
    }
    const auto consume_line = [&](const std::string& line) {
      std::uint64_t line_projection = 0;
      std::uint64_t line_phase_memory = initial_decode_memory;
      if (!CheckedHeapReadMemoryMultiply(
              static_cast<std::uint64_t>(line.size()) + 1, 128,
              &line_projection) ||
          !CheckedHeapReadMemoryAdd(retained_text_row_projection,
                                    &line_phase_memory) ||
          !CheckedHeapReadMemoryAdd(line_projection,
                                    &line_phase_memory) ||
          !ObserveBoundedHeapReadMemory(control, line_phase_memory)) {
        if (control->refusal_detail.empty()) {
          control->refusal_detail =
              "heap_read_text_memory_receipt_overflow";
        }
        return false;
      }
      const auto fields = SplitTabs(line);
      if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
          fields[1] != "ROW_VERSION") {
        return true;
      }
      CrudRowVersionRecord row;
      row.creator_tx = ParseU64(fields[2]);
      row.event_sequence = ParseU64(fields[3]);
      row.sequence = row.event_sequence;
      row.table_uuid = fields[4];
      row.row_uuid = fields[5];
      row.version_uuid = fields[6];
      row.deleted = fields[7] == "1";
      row.previous_version_uuid = fields[8];
      row.previous_sequence = ParseU64(fields[9]);
      row.values =
          DecodeCrudPairsWithKeyCache(fields[10], &row_value_key_cache);
      if (fields.size() >= 12) { row.temporary_session_uuid = fields[11]; }
      if (!AdmitBoundedScopedRow(control, row)) { return false; }
      // line_projection is a conservative bound for the transient tab/hex
      // decoder working set.  It is not retained once the decoded row has
      // been materialized.  Charging that transient estimate for every row
      // made the receipt grow by 128 times the complete relation payload and
      // rejected small, bounded relations despite a much smaller live set.
      // Account the exact owned row carriers instead; the vector's reserved
      // structural storage is already included in
      // retained_text_row_projection.
      std::uint64_t retained_row_bytes = 0;
      if (!AccountHeapReadRowDynamicMemoryBytes(row, &retained_row_bytes) ||
          !CheckedHeapReadMemoryAdd(retained_row_bytes,
                                    &retained_text_row_projection)) {
        control->refusal_detail =
            "heap_read_text_memory_receipt_overflow";
        return false;
      }
      decoded_rows.push_back(std::move(row));
      return true;
    };
    constexpr std::size_t kReadChunkBytes = 64 * 1024;
    char chunk[kReadChunkBytes];
    std::string line;
    std::uint64_t actual_text_bytes = 0;
    bool reached_eof = false;
    while (!reached_eof) {
      if (BoundedScopedReadCancelled(control)) { return false; }
      std::uint64_t line_append_phase_memory = initial_decode_memory;
      if (!CheckedHeapReadMemoryAdd(retained_text_row_projection,
                                    &line_append_phase_memory) ||
          !CheckedHeapReadMemoryAdd(authorized_text_bytes,
                                    &line_append_phase_memory) ||
          !ObserveBoundedHeapReadMemory(control,
                                        line_append_phase_memory)) {
        if (control->refusal_detail.empty()) {
          control->refusal_detail =
              "heap_read_text_memory_receipt_overflow";
        }
        return false;
      }
      const std::uint64_t remaining =
          authorized_text_bytes - actual_text_bytes;
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
        control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
        control->refusal_detail = "heap_read_scoped_text_grew_during_read";
        return false;
      }
      actual_text_bytes += static_cast<std::uint64_t>(read_count);
      if (!AccountHeapStorageBytes(
              control, static_cast<std::uint64_t>(read_count))) {
        return false;
      }
      std::size_t begin = 0;
      const std::size_t count = static_cast<std::size_t>(read_count);
      for (std::size_t index = 0; index < count; ++index) {
        if (chunk[index] != '\n') { continue; }
        line.append(chunk + begin, index - begin);
        if (!consume_line(line)) { return false; }
        line.clear();
        begin = index + 1;
      }
      if (begin < count) { line.append(chunk + begin, count - begin); }
      if (input.bad()) {
        control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
        control->refusal_detail = "heap_read_scoped_text_read_failed";
        return false;
      }
      if (read_count < static_cast<std::streamsize>(requested)) {
        if (!input.eof()) {
          control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
          control->refusal_detail = "heap_read_scoped_text_read_failed";
          return false;
        }
        reached_eof = true;
      }
    }
    if (actual_text_bytes != authorized_text_bytes) {
      control->failure_category = MgaHeapReadFailureCategoryV1::kStorage;
      control->refusal_detail = "heap_read_scoped_text_changed_during_read";
      return false;
    }
    if (!line.empty() && !consume_line(line)) { return false; }
    const auto parent_memory = decode_parent_memory();
    const auto row_memory = HeapReadRowVectorMemoryBytes(decoded_rows);
    std::uint64_t phase_memory = 0;
    if (!parent_memory.has_value() || !row_memory.has_value() ||
        !CheckedHeapReadMemoryAdd(*parent_memory, &phase_memory) ||
        !CheckedHeapReadMemoryAdd(*row_memory, &phase_memory) ||
        !AccountHeapReadOwnedString(line, &phase_memory) ||
        !ObserveBoundedHeapReadMemory(control, phase_memory)) {
      if (control->refusal_detail.empty()) {
        control->refusal_detail =
            "heap_read_text_memory_receipt_overflow";
      }
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
  *rows = std::move(decoded_rows);
  return true;
}



void ClearScopedDecodedRowCache() {
  const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
  ScopedDecodedRowCache().clear();
}

}  // namespace scratchbird::engine::internal_api
