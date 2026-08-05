// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"

#include "api_diagnostics.hpp"
#include "agents/index_garbage_cleanup_agent.hpp"
#include "descriptor_value_runtime.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "query/plan_api.hpp"
#include "secondary_index_delta_merge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;

constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr const char* kDescriptorMagic = "SBMGADESC1";
constexpr const char* kEventSequenceAllocatorMagic = "SBMGAEVSEQ1";
constexpr std::string_view kLineHexFieldPrefix = "SBHEX:";
constexpr std::size_t kMgaLargeValueChunkBytes = 2048;

using scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase;
using scratchbird::transaction::mga::LookupLocalTransaction;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionState;

std::string RowStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_row_versions";
}

std::string MetadataStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_metadata";
}

std::string IndexStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_index_entries";
}

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

std::string ScopedIndexStorePath(const EngineRequestContext& context,
                                 const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".indexes";
}

std::string ScopedIndexBinaryStorePath(const EngineRequestContext& context,
                                       const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".indexes.sbnx";
}

std::string ScopedSummaryStorePath(const EngineRequestContext& context,
                                   const std::string& table_uuid) {
  return ScopedRelationStoreRoot(context) + "/" +
         ScopedRelationSegmentName(table_uuid) + ".summary";
}

std::string SecondaryIndexDeltaLedgerStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_secondary_index_delta_ledger";
}

std::string EventSequenceAllocatorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_event_sequence_allocator";
}

std::string DescriptorStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_descriptors";
}

std::string LargeValueStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_large_values";
}

std::string SavepointStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_savepoints";
}

std::mutex& ScopedRelationSummaryMutex() {
  static std::mutex mutex;
  return mutex;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return fields;
}

bool AppendLine(const std::string& path, const std::string& line) {
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  out << line << '\n';
  out.flush();
  return static_cast<bool>(out);
}

bool AppendLines(const std::string& path,
                 const std::vector<std::string>& lines,
                 std::uint64_t* stream_opens,
                 std::uint64_t* stream_flushes) {
  if (lines.empty()) { return true; }
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  if (stream_opens != nullptr) { ++(*stream_opens); }
  std::size_t buffer_bytes = 0;
  for (const auto& line : lines) {
    buffer_bytes += line.size() + 1;
  }
  std::string buffer;
  buffer.reserve(buffer_bytes);
  for (const auto& line : lines) {
    buffer.append(line);
    buffer.push_back('\n');
  }
  if (!buffer.empty()) {
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  }
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
}

bool AppendBuffer(const std::string& path,
                  const std::string& buffer,
                  std::uint64_t* stream_opens,
                  std::uint64_t* stream_flushes) {
  if (buffer.empty()) { return true; }
  if (path.empty()) { return false; }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) { return false; }
  if (stream_opens != nullptr) { ++(*stream_opens); }
  out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
}

bool AppendDeferredEventSequenceAllocatorLines(
    const EngineRequestContext& context,
    std::vector<std::string>* lines,
    MgaRelationHotAppendCounters* counters) {
  if (lines == nullptr || lines->empty()) { return true; }
  const bool ok = AppendLines(EventSequenceAllocatorStorePath(context),
                              *lines,
                              counters == nullptr
                                  ? nullptr
                                  : &counters->allocator_stream_opens,
                              counters == nullptr
                                  ? nullptr
                                  : &counters->allocator_stream_flushes);
  if (ok && counters != nullptr) {
    counters->allocator_range_records_appended +=
        static_cast<std::uint64_t>(lines->size());
  }
  if (ok) {
    lines->clear();
  }
  return ok;
}

bool AppendScopedRelationBuffer(const std::string& path,
                                const std::string& buffer,
                                std::uint64_t* stream_opens,
                                std::uint64_t* stream_flushes) {
  if (path.empty()) { return false; }
  if (buffer.empty()) { return true; }
  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(),
                                      ignored);
  return AppendBuffer(path, buffer, stream_opens, stream_flushes);
}

struct ScopedRelationWriteTicket {
  std::string path;
  bool ok = false;
  std::uint64_t stream_opens = 0;
  std::uint64_t stream_flushes = 0;
};

struct ScopedRelationAppendResult {
  bool ok = false;
  std::uint64_t stream_opens = 0;
  std::uint64_t stream_flushes = 0;
  std::uint64_t write_batches = 0;
  std::uint64_t write_tickets_issued = 0;
  std::uint64_t write_tickets_completed = 0;
  std::uint64_t write_worker_count = 0;
};

ScopedRelationWriteTicket AppendScopedRelationBufferTicket(
    const std::string& path,
    const std::string& buffer) {
  ScopedRelationWriteTicket ticket;
  ticket.path = path;
  ticket.ok = AppendScopedRelationBuffer(ticket.path,
                                         buffer,
                                         &ticket.stream_opens,
                                         &ticket.stream_flushes);
  return ticket;
}

bool AppendScopedRelationLinesParallel(
    const std::map<std::string, std::string>& pending,
    std::uint64_t* stream_opens,
    std::uint64_t* stream_flushes,
    std::uint64_t* write_batches,
    std::uint64_t* write_tickets_issued,
    std::uint64_t* write_tickets_completed,
    std::uint64_t* write_worker_count) {
  if (pending.empty()) {
    return true;
  }
  if (write_batches != nullptr) { ++(*write_batches); }
  if (write_tickets_issued != nullptr) {
    *write_tickets_issued += static_cast<std::uint64_t>(pending.size());
  }
  if (write_worker_count != nullptr) {
    *write_worker_count = std::max(*write_worker_count,
                                   static_cast<std::uint64_t>(pending.size()));
  }
  if (pending.size() == 1) {
    const auto& [path, buffer] = *pending.begin();
    const auto ticket = AppendScopedRelationBufferTicket(path, buffer);
    if (stream_opens != nullptr) { *stream_opens += ticket.stream_opens; }
    if (stream_flushes != nullptr) { *stream_flushes += ticket.stream_flushes; }
    if (write_tickets_completed != nullptr) { ++(*write_tickets_completed); }
    return ticket.ok;
  }
  std::vector<std::future<ScopedRelationWriteTicket>> futures;
  futures.reserve(pending.size());
  for (const auto& [path, buffer] : pending) {
    const auto* path_ptr = &path;
    const auto* buffer_ptr = &buffer;
    futures.push_back(std::async(std::launch::async,
                                 [path_ptr, buffer_ptr]() {
                                   return AppendScopedRelationBufferTicket(
                                       *path_ptr,
                                       *buffer_ptr);
                                 }));
  }
  bool ok = true;
  for (auto& future : futures) {
    const auto ticket = future.get();
    if (stream_opens != nullptr) { *stream_opens += ticket.stream_opens; }
    if (stream_flushes != nullptr) { *stream_flushes += ticket.stream_flushes; }
    if (write_tickets_completed != nullptr) { ++(*write_tickets_completed); }
    ok = ok && ticket.ok;
  }
  return ok;
}

ScopedRelationAppendResult AppendScopedRelationLinesWithCounters(
    const std::map<std::string, std::string>& pending) {
  ScopedRelationAppendResult result;
  result.ok = AppendScopedRelationLinesParallel(
      pending,
      &result.stream_opens,
      &result.stream_flushes,
      &result.write_batches,
      &result.write_tickets_issued,
      &result.write_tickets_completed,
      &result.write_worker_count);
  return result;
}

void AddScopedRelationAppendCounters(
    const ScopedRelationAppendResult& result,
    std::uint64_t* stream_opens,
    std::uint64_t* stream_flushes,
    std::uint64_t* write_batches,
    std::uint64_t* write_tickets_issued,
    std::uint64_t* write_tickets_completed,
    std::uint64_t* write_worker_count) {
  if (stream_opens != nullptr) { *stream_opens += result.stream_opens; }
  if (stream_flushes != nullptr) { *stream_flushes += result.stream_flushes; }
  if (write_batches != nullptr) { *write_batches += result.write_batches; }
  if (write_tickets_issued != nullptr) {
    *write_tickets_issued += result.write_tickets_issued;
  }
  if (write_tickets_completed != nullptr) {
    *write_tickets_completed += result.write_tickets_completed;
  }
  if (write_worker_count != nullptr) {
    *write_worker_count = std::max(*write_worker_count,
                                   result.write_worker_count);
  }
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) { return lines; }
  std::error_code ignored;
  const auto bytes = std::filesystem::file_size(path, ignored);
  if (!ignored && bytes != static_cast<std::uintmax_t>(-1)) {
    lines.reserve(static_cast<std::size_t>(std::max<std::uintmax_t>(1, bytes / 128)));
  }
  std::string line;
  while (std::getline(in, line)) { lines.push_back(line); }
  return lines;
}

bool FileExistsAndNotEmpty(const std::string& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored) &&
         std::filesystem::file_size(path, ignored) != 0;
}

std::vector<std::string> ReadScopedRelationLinesForTables(
    const EngineRequestContext& context,
    const std::set<std::string>& table_uuids,
    bool row_store,
    bool* used_segments) {
  std::vector<std::string> lines;
  bool used = false;
  for (const auto& table_uuid : table_uuids) {
    const std::string path = row_store
                                 ? ScopedRowStorePath(context, table_uuid)
                                 : ScopedIndexStorePath(context, table_uuid);
    if (!FileExistsAndNotEmpty(path)) {
      continue;
    }
    used = true;
    auto segment_lines = ReadLines(path);
    lines.reserve(lines.size() + segment_lines.size());
    lines.insert(lines.end(),
                 std::make_move_iterator(segment_lines.begin()),
                 std::make_move_iterator(segment_lines.end()));
  }
  if (used_segments != nullptr) {
    *used_segments = used;
  }
  return lines;
}

std::set<std::string> DiscoverScopedRelationTableUuids(
    const EngineRequestContext& context) {
  std::set<std::string> table_uuids;
  const std::filesystem::path root = ScopedRelationStoreRoot(context);
  std::error_code ignored;
  if (!std::filesystem::exists(root, ignored)) {
    return table_uuids;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ignored)) {
    if (ignored) { break; }
    if (!entry.is_regular_file(ignored)) {
      ignored.clear();
      continue;
    }
    std::string name = entry.path().filename().string();
    for (const std::string_view suffix :
         {".rows.sbnr", ".indexes.sbnx", ".rows", ".indexes", ".summary"}) {
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        table_uuids.insert(name.substr(0, name.size() - suffix.size()));
        break;
      }
    }
    ignored.clear();
  }
  return table_uuids;
}

std::vector<idx::byte> ReadBinaryFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { return {}; }
  std::vector<idx::byte> bytes;
  const auto begin = std::istreambuf_iterator<char>(in);
  const auto end = std::istreambuf_iterator<char>();
  for (auto it = begin; it != end; ++it) {
    bytes.push_back(static_cast<idx::byte>(*it));
  }
  return bytes;
}

struct ScopedRelationSummary {
  bool trusted = false;
  bool malformed = false;
  std::uint64_t row_version_count = 0;
  std::uint64_t tombstone_count = 0;
  std::uint64_t update_count = 0;
};

void ReserveAmortizedAppendCapacity(std::string* out, std::size_t extra);

constexpr std::string_view kScopedRowBinaryBatchMagic = "SBMRBIN1";
constexpr std::uint16_t kScopedRowBinaryVersion = 3;
constexpr std::uint16_t kScopedRowBinaryLegacyTypedVersion = 2;
constexpr std::uint16_t kScopedRowBinaryNativePacketVersion = 4;
constexpr std::string_view kScopedIndexBinaryBatchMagic = "SBMIBIN1";
constexpr std::uint16_t kScopedIndexBinaryVersion = 1;

void AppendBinaryU8(std::string* out, std::uint8_t value) {
  if (out == nullptr) { return; }
  out->push_back(static_cast<char>(value));
}

void AppendBinaryU16(std::string* out, std::uint16_t value) {
  if (out == nullptr) { return; }
  out->push_back(static_cast<char>(value & 0xffu));
  out->push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void AppendBinaryU32(std::string* out, std::uint32_t value) {
  if (out == nullptr) { return; }
  for (std::size_t index = 0; index < 4; ++index) {
    out->push_back(static_cast<char>((value >> (index * 8u)) & 0xffu));
  }
}

void AppendBinaryU64(std::string* out, std::uint64_t value) {
  if (out == nullptr) { return; }
  for (std::size_t index = 0; index < 8; ++index) {
    out->push_back(static_cast<char>((value >> (index * 8u)) & 0xffu));
  }
}

bool AppendBinaryString(std::string* out, std::string_view value) {
  if (out == nullptr ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  AppendBinaryU32(out, static_cast<std::uint32_t>(value.size()));
  out->append(value.data(), value.size());
  return true;
}

const std::array<std::int8_t, 256>& BinaryUuidHexTable() {
  static const std::array<std::int8_t, 256> table = [] {
    std::array<std::int8_t, 256> values{};
    values.fill(-1);
    for (int index = 0; index < 10; ++index) {
      values[static_cast<unsigned char>('0' + index)] =
          static_cast<std::int8_t>(index);
    }
    for (int index = 0; index < 6; ++index) {
      values[static_cast<unsigned char>('a' + index)] =
          static_cast<std::int8_t>(10 + index);
      values[static_cast<unsigned char>('A' + index)] =
          static_cast<std::int8_t>(10 + index);
    }
    return values;
  }();
  return table;
}

bool AppendBinaryUuidText(std::string* out, const std::string& text) {
  if (out == nullptr) { return false; }
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  static constexpr std::array<std::uint8_t, 32> kHexPositions = {
      0, 1, 2, 3, 4, 5, 6, 7,
      9, 10, 11, 12,
      14, 15, 16, 17,
      19, 20, 21, 22,
      24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
  const auto& table = BinaryUuidHexTable();
  std::array<char, 16> bytes{};
  for (std::size_t byte_index = 0; byte_index < bytes.size(); ++byte_index) {
    const int hi =
        table[static_cast<unsigned char>(text[kHexPositions[byte_index * 2]])];
    const int lo =
        table[static_cast<unsigned char>(text[kHexPositions[byte_index * 2 + 1]])];
    if (hi < 0 || lo < 0) { return false; }
    bytes[byte_index] = static_cast<char>((hi << 4) | lo);
  }
  out->append(bytes.data(), bytes.size());
  return true;
}

bool ReadBinaryU8(const std::vector<idx::byte>& bytes,
                  std::size_t* offset,
                  std::uint8_t* out) {
  if (offset == nullptr || out == nullptr || *offset + 1 > bytes.size()) {
    return false;
  }
  *out = static_cast<std::uint8_t>(bytes[*offset]);
  ++(*offset);
  return true;
}

bool ReadBinaryU16(const std::vector<idx::byte>& bytes,
                   std::size_t* offset,
                   std::uint16_t* out) {
  if (offset == nullptr || out == nullptr || *offset + 2 > bytes.size()) {
    return false;
  }
  std::uint16_t value = 0;
  for (std::size_t index = 0; index < 2; ++index) {
    value |= static_cast<std::uint16_t>(bytes[*offset + index])
             << (index * 8u);
  }
  *offset += 2;
  *out = value;
  return true;
}

bool ReadBinaryU32(const std::vector<idx::byte>& bytes,
                   std::size_t* offset,
                   std::uint32_t* out) {
  if (offset == nullptr || out == nullptr || *offset + 4 > bytes.size()) {
    return false;
  }
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[*offset + index])
             << (index * 8u);
  }
  *offset += 4;
  *out = value;
  return true;
}

bool ReadBinaryU64(const std::vector<idx::byte>& bytes,
                   std::size_t* offset,
                   std::uint64_t* out) {
  if (offset == nullptr || out == nullptr || *offset + 8 > bytes.size()) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[*offset + index])
             << (index * 8u);
  }
  *offset += 8;
  *out = value;
  return true;
}

bool ReadBinaryString(const std::vector<idx::byte>& bytes,
                      std::size_t* offset,
                      std::string* out) {
  if (offset == nullptr || out == nullptr) { return false; }
  std::uint32_t size = 0;
  if (!ReadBinaryU32(bytes, offset, &size) || *offset + size > bytes.size()) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return true;
}

bool ReadBinaryUuidText(const std::vector<idx::byte>& bytes,
                        std::size_t* offset,
                        std::string* out) {
  if (offset == nullptr || out == nullptr || *offset + 16 > bytes.size()) {
    return false;
  }
  scratchbird::core::platform::Uuid uuid;
  std::copy_n(bytes.data() + *offset, uuid.bytes.size(), uuid.bytes.begin());
  *offset += uuid.bytes.size();
  *out = scratchbird::core::uuid::UuidToString(uuid);
  return true;
}

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
  if (!typed.descriptor.encoded_descriptor.empty()) {
    const std::string_view encoded = typed.descriptor.encoded_descriptor;
    constexpr std::string_view prefix = "type=";
    if (encoded.rfind(prefix, 0) == 0) {
      return std::string(encoded.substr(prefix.size()));
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
    bytes += 8 + 8 + 4 + rows.front().table_uuid.size() + 4 +
             rows.front().temporary_session_uuid.size() + 1;
  }
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    if (compact_batch) {
      bytes += 16 + 16;
    } else {
      bytes += 8 + 8 + 8 + 1;
      bytes += 4 + row.table_uuid.size();
      bytes += 4 + row.row_uuid.size();
      bytes += 4 + row.version_uuid.size();
      bytes += 4 + row.previous_version_uuid.size();
      bytes += 4 + row.temporary_session_uuid.size();
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
      field_order.empty() ||
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
                                : kScopedRowBinaryLegacyTypedVersion);
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
    if (!AppendBinaryString(out, rows.front().table_uuid) ||
        !AppendBinaryString(out, rows.front().temporary_session_uuid)) {
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
      if (!AppendBinaryUuidText(out, row.row_uuid) ||
          !AppendBinaryUuidText(out, row.version_uuid)) {
        return false;
      }
      ++event_sequence;
    } else {
      AppendBinaryU64(out, row.creator_tx);
      AppendBinaryU64(out, event_sequence++);
      AppendBinaryU64(out, row.previous_sequence);
      AppendBinaryU8(out, row.deleted ? 1u : 0u);
      if (!AppendBinaryString(out, row.table_uuid) ||
          !AppendBinaryString(out, row.row_uuid) ||
          !AppendBinaryString(out, row.version_uuid) ||
          !AppendBinaryString(out, row.previous_version_uuid) ||
          !AppendBinaryString(out, row.temporary_session_uuid)) {
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
      if (!AppendBinaryString(out, ScopedRowBinaryPayloadView(typed))) {
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
    const std::string& table_uuid,
    const std::string& temporary_session_uuid) {
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
  bytes += 8 + 8 + 4 + table_uuid.size() + 4 +
           temporary_session_uuid.size() + 1;
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
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> field_order,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || row_identities.empty() ||
      typed_rows.size() != row_identities.size() ||
      table_uuid.empty() || field_order.empty() ||
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
  if (!AppendBinaryString(out, table_uuid) ||
      !AppendBinaryString(out, temporary_session_uuid)) {
    return false;
  }
  AppendBinaryU8(out, 0);
  const std::size_t null_bitmap_bytes = (field_order.size() + 7u) / 8u;
  std::vector<std::uint8_t> null_bitmap(null_bitmap_bytes);
  for (std::size_t row_index = 0; row_index < row_identities.size();
       ++row_index) {
    const auto& row = row_identities[row_index];
    const auto& typed_row = typed_rows[row_index];
    if (row.row_uuid.empty() || row.version_uuid.empty() ||
        typed_row.fields.size() != field_order.size()) {
      return false;
    }
    if (!AppendBinaryUuidText(out, row.row_uuid) ||
        !AppendBinaryUuidText(out, row.version_uuid)) {
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
      if (!AppendBinaryString(out, ScopedRowBinaryPayloadView(typed))) {
        return false;
      }
    }
  }
  return true;
}

std::size_t ScopedRowNativePacketIdentityBatchEstimateBytes(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const EngineNativeRowPacketFrame& frame,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid) {
  std::size_t bytes = kScopedRowBinaryBatchMagic.size() + 2 + 2 + 4 + 8;
  for (const auto& field : frame.field_order) {
    bytes += 4 + field.size();
  }
  bytes += frame.column_type_tags.size();
  bytes += 8 + 8 + 4 + table_uuid.size() + 4 +
           temporary_session_uuid.size() + 1;
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
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    const EngineNativeRowPacketFrame& frame,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || row_identities.empty() || !frame.present ||
      frame.version != 2 ||
      table_uuid.empty() || table_uuid == "unknown" ||
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
  if (!AppendBinaryString(out, table_uuid) ||
      !AppendBinaryString(out, temporary_session_uuid)) {
    return false;
  }
  AppendBinaryU8(out, 0);
  for (std::size_t row_index = 0; row_index < row_identities.size();
       ++row_index) {
    const auto& row = row_identities[row_index];
    if (row.row_uuid.empty() || row.version_uuid.empty()) {
      return false;
    }
    const std::size_t row_offset = frame.row_offsets[row_index];
    const std::size_t row_size = frame.row_sizes[row_index];
    if (row_size == 0 || row_offset > frame.packet_bytes.size() ||
        row_size > frame.packet_bytes.size() - row_offset) {
      return false;
    }
    if (!AppendBinaryUuidText(out, row.row_uuid) ||
        !AppendBinaryUuidText(out, row.version_uuid)) {
      return false;
    }
    out->append(reinterpret_cast<const char*>(frame.packet_bytes.data() +
                                             row_offset),
                row_size);
  }
  return true;
}

struct BoundedScopedRowReadControl {
  std::uint64_t maximum_row_versions = 0;
  std::uint64_t maximum_bytes = 0;
  std::function<bool()> cancellation_requested;
  std::uint64_t decoded_row_versions = 0;
  std::uint64_t decoded_bytes = 0;
  bool cancellation_observed = false;
  std::string refusal_detail;
};

bool BoundedScopedReadCancelled(BoundedScopedRowReadControl* control) {
  if (control == nullptr || !control->cancellation_requested) { return false; }
  if (!control->cancellation_requested()) { return false; }
  control->cancellation_observed = true;
  control->refusal_detail = "heap_read_cancelled";
  return true;
}

bool AccountBoundedScopedBytes(BoundedScopedRowReadControl* control,
                               const std::uint64_t bytes) {
  if (control == nullptr) { return true; }
  if (control->decoded_bytes > control->maximum_bytes ||
      bytes > control->maximum_bytes - control->decoded_bytes) {
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
      control->refusal_detail = "heap_read_scoped_binary_size_overflow";
    }
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    control->refusal_detail = "heap_read_scoped_binary_open_failed";
    return false;
  }
  bytes->clear();
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
    input.read(chunk, static_cast<std::streamsize>(requested));
    const std::streamsize read_count = input.gcount();
    if (read_count < 0 ||
        static_cast<std::uint64_t>(read_count) > remaining) {
      control->refusal_detail = "heap_read_scoped_binary_grew_during_read";
      return false;
    }
    if (read_count > 0) {
      const std::size_t old_size = bytes->size();
      const std::size_t appended = static_cast<std::size_t>(read_count);
      if (appended > std::numeric_limits<std::size_t>::max() - old_size) {
        control->refusal_detail = "heap_read_scoped_binary_size_overflow";
        return false;
      }
      bytes->resize(old_size + appended);
      std::memcpy(bytes->data() + old_size, chunk, appended);
      actual_file_bytes += static_cast<std::uint64_t>(read_count);
    }
    if (input.bad()) {
      control->refusal_detail = "heap_read_scoped_binary_read_failed";
      return false;
    }
    if (read_count < static_cast<std::streamsize>(requested)) {
      if (input.eof()) {
        if (actual_file_bytes != authorized_file_bytes) {
          control->refusal_detail =
              "heap_read_scoped_binary_changed_during_read";
          return false;
        }
        return true;
      }
      control->refusal_detail = "heap_read_scoped_binary_read_failed";
      return false;
    }
  }
}

bool AdmitBoundedScopedRow(BoundedScopedRowReadControl* control,
                           const CrudRowVersionRecord& row) {
  if (control == nullptr) { return true; }
  if (BoundedScopedReadCancelled(control)) { return false; }
  if (control->decoded_row_versions >= control->maximum_row_versions) {
    control->refusal_detail = "heap_read_maximum_row_versions_exceeded";
    return false;
  }
  std::uint64_t decoded_bytes = 0;
  const auto add_decoded_size = [&](const std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - decoded_bytes) {
      control->refusal_detail = "heap_read_decoded_byte_count_overflow";
      return false;
    }
    decoded_bytes += static_cast<std::uint64_t>(bytes);
    return true;
  };
  if (!add_decoded_size(row.table_uuid.size()) ||
      !add_decoded_size(row.row_uuid.size()) ||
      !add_decoded_size(row.version_uuid.size()) ||
      !add_decoded_size(row.temporary_session_uuid.size()) ||
      !add_decoded_size(row.previous_version_uuid.size())) {
    return false;
  }
  for (const auto& [key, value] : row.values) {
    if (!add_decoded_size(key.size()) || !add_decoded_size(value.size())) {
      return false;
    }
  }
  if (!AccountBoundedScopedBytes(control, decoded_bytes)) { return false; }
  ++control->decoded_row_versions;
  return true;
}

bool DecodeScopedRowBinaryStore(
    const std::string& path,
    std::vector<CrudRowVersionRecord>* rows,
    ScopedRelationSummary* summary,
    BoundedScopedRowReadControl* control = nullptr,
    const std::uint64_t authorized_file_bytes = 0) {
  if (rows == nullptr || summary == nullptr) { return false; }
  if (BoundedScopedReadCancelled(control)) { return false; }
  if (!FileExistsAndNotEmpty(path)) {
    if (control != nullptr && authorized_file_bytes != 0) {
      control->refusal_detail =
          "heap_read_scoped_binary_changed_during_read";
      return false;
    }
    summary->trusted = true;
    return true;
  }
  std::vector<idx::byte> bytes;
  if (control == nullptr) {
    bytes = ReadBinaryFile(path);
  } else if (!ReadBinaryFileBounded(path,
                                    authorized_file_bytes,
                                    control,
                                    &bytes)) {
    return false;
  }
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
        (version != 1 && version != kScopedRowBinaryLegacyTypedVersion &&
         version != kScopedRowBinaryVersion &&
         version != kScopedRowBinaryNativePacketVersion) ||
        flags != 0 ||
        column_count == 0 || column_count > 4096) {
      summary->malformed = true;
      summary->trusted = false;
      return false;
    }
    if (row_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() /
                                   column_count)) {
      summary->malformed = true;
      summary->trusted = false;
      return false;
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
    } else if (version >= kScopedRowBinaryLegacyTypedVersion) {
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
    std::string compact_table_uuid;
    std::string compact_temporary_session_uuid;
    std::uint8_t compact_flags = 0;
    const bool compact_batch = version >= kScopedRowBinaryVersion;
    if (compact_batch) {
      if (!ReadBinaryU64(bytes, &offset, &compact_first_event_sequence) ||
          !ReadBinaryU64(bytes, &offset, &compact_creator_tx) ||
          !ReadBinaryString(bytes, &offset, &compact_table_uuid) ||
          !ReadBinaryString(bytes, &offset, &compact_temporary_session_uuid) ||
          !ReadBinaryU8(bytes, &offset, &compact_flags) ||
          compact_table_uuid.empty() || compact_flags != 0) {
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
        if (!ReadBinaryUuidText(bytes, &offset, &row.row_uuid) ||
            !ReadBinaryUuidText(bytes, &offset, &row.version_uuid) ||
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
            !ReadBinaryString(bytes, &offset, &row.table_uuid) ||
            !ReadBinaryString(bytes, &offset, &row.row_uuid) ||
            !ReadBinaryString(bytes, &offset, &row.version_uuid) ||
            !ReadBinaryString(bytes, &offset, &row.previous_version_uuid) ||
            !ReadBinaryString(bytes, &offset, &row.temporary_session_uuid) ||
            offset + null_bitmap_bytes > bytes.size()) {
          summary->malformed = true;
          summary->trusted = false;
          return false;
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
              if (!ReadBinaryString(bytes, &offset, &value)) {
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
          if (!ReadBinaryString(bytes, &offset, &value)) {
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
      if (!row.previous_version_uuid.empty()) { ++summary->update_count; }
      if (!AdmitBoundedScopedRow(control, row)) { return false; }
      rows->push_back(std::move(row));
    }
  }
  summary->trusted = true;
  return true;
}

bool AppendScopedExactIndexBinaryBatch(
    std::string* out,
    const MgaExactIndexEntryAppendBatch& batch,
    std::uint64_t creator_tx,
    std::uint64_t first_event_sequence) {
  if (out == nullptr || batch.entries.empty()) {
    return false;
  }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  if (table_uuid.empty() || batch.index.index_uuid.empty()) {
    return false;
  }
  std::size_t estimate = kScopedIndexBinaryBatchMagic.size() + 2 + 2 + 8 + 8 +
                         8 + 24 + table_uuid.size() +
                         batch.index.index_uuid.size() +
                         batch.index.column_name.size() +
                         batch.index.family.size();
  for (const auto& entry : batch.entries) {
    estimate += 16 + entry.encoded_key.size() + entry.payload_value.size() +
                entry.row_uuid.size() + entry.version_uuid.size();
  }
  ReserveAmortizedAppendCapacity(out, estimate);
  out->append(kScopedIndexBinaryBatchMagic.data(),
              kScopedIndexBinaryBatchMagic.size());
  AppendBinaryU16(out, kScopedIndexBinaryVersion);
  AppendBinaryU16(out, 0);
  AppendBinaryU64(out, static_cast<std::uint64_t>(batch.entries.size()));
  AppendBinaryU64(out, creator_tx);
  AppendBinaryU64(out, first_event_sequence);
  if (!AppendBinaryString(out, table_uuid) ||
      !AppendBinaryString(out, batch.index.index_uuid) ||
      !AppendBinaryString(out, batch.index.column_name) ||
      !AppendBinaryString(out, batch.index.family) ||
      !AppendBinaryString(out, "exact")) {
    return false;
  }
  for (const auto& entry : batch.entries) {
    if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
        entry.version_uuid.empty()) {
      return false;
    }
    if (!AppendBinaryString(out, entry.encoded_key) ||
        !AppendBinaryString(out, entry.payload_value) ||
        !AppendBinaryString(out, entry.row_uuid) ||
        !AppendBinaryString(out, entry.version_uuid)) {
      return false;
    }
  }
  return true;
}

bool DecodeScopedIndexBinaryStore(
    const std::string& path,
    std::vector<CrudIndexEntryRecord>* entries) {
  if (entries == nullptr) {
    return false;
  }
  if (!FileExistsAndNotEmpty(path)) {
    return true;
  }
  const std::vector<idx::byte> bytes = ReadBinaryFile(path);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (offset + kScopedIndexBinaryBatchMagic.size() > bytes.size() ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + offset),
                         kScopedIndexBinaryBatchMagic.size()) !=
            kScopedIndexBinaryBatchMagic) {
      return false;
    }
    offset += kScopedIndexBinaryBatchMagic.size();
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t creator_tx = 0;
    std::uint64_t first_event_sequence = 0;
    if (!ReadBinaryU16(bytes, &offset, &version) ||
        !ReadBinaryU16(bytes, &offset, &flags) ||
        !ReadBinaryU64(bytes, &offset, &entry_count) ||
        !ReadBinaryU64(bytes, &offset, &creator_tx) ||
        !ReadBinaryU64(bytes, &offset, &first_event_sequence) ||
        version != kScopedIndexBinaryVersion ||
        flags != 0 ||
        entry_count >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return false;
    }
    std::string table_uuid;
    std::string index_uuid;
    std::string column_name;
    std::string family;
    std::string entry_kind;
    if (!ReadBinaryString(bytes, &offset, &table_uuid) ||
        !ReadBinaryString(bytes, &offset, &index_uuid) ||
        !ReadBinaryString(bytes, &offset, &column_name) ||
        !ReadBinaryString(bytes, &offset, &family) ||
        !ReadBinaryString(bytes, &offset, &entry_kind) ||
        table_uuid.empty() || index_uuid.empty() || entry_kind.empty()) {
      return false;
    }
    entries->reserve(entries->size() + static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = creator_tx;
      entry.event_sequence = first_event_sequence + index;
      entry.sequence = entry.event_sequence;
      entry.table_uuid = table_uuid;
      entry.index_uuid = index_uuid;
      entry.column_name = column_name;
      entry.family = family;
      entry.entry_kind = entry_kind;
      if (!ReadBinaryString(bytes, &offset, &entry.key_value) ||
          !ReadBinaryString(bytes, &offset, &entry.payload_value) ||
          !ReadBinaryString(bytes, &offset, &entry.row_uuid) ||
          !ReadBinaryString(bytes, &offset, &entry.version_uuid) ||
          entry.key_value.empty() || entry.row_uuid.empty() ||
          entry.version_uuid.empty()) {
        return false;
      }
      entries->push_back(std::move(entry));
    }
  }
  return true;
}

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) { line += '\t'; }
    line += fields[i];
  }
  return line;
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

constexpr std::size_t kRowVersionStoreLineReserveSlackBytes = 384;

void AppendRowVersionStoreLine(std::string* out,
                               const CrudRowVersionRecord& row,
                               std::uint64_t event_sequence_override,
                               const std::vector<std::pair<std::string, std::string>>&
                                   values,
                               const std::vector<std::string>* encoded_keys =
                                   nullptr) {
  if (out == nullptr) { return; }
  const std::size_t fixed_size_floor =
      128 + row.table_uuid.size() + row.row_uuid.size() +
      row.version_uuid.size() + row.previous_version_uuid.size() +
      row.temporary_session_uuid.size();
  const std::size_t available =
      out->capacity() > out->size() ? out->capacity() - out->size() : 0;
  if (available < fixed_size_floor + kRowVersionStoreLineReserveSlackBytes) {
    ReserveAmortizedAppendCapacity(out,
                                   fixed_size_floor +
                                       EncodedCrudPairsSize(values));
  }
  bool first = true;
  AppendLineField(out, &first, kRowStoreMagic);
  AppendLineField(out, &first, "ROW_VERSION");
  AppendLineU64Field(out, &first, row.creator_tx);
  AppendLineU64Field(out, &first, event_sequence_override);
  AppendLineField(out, &first, row.table_uuid);
  AppendLineField(out, &first, row.row_uuid);
  AppendLineField(out, &first, row.version_uuid);
  AppendLineField(out, &first, row.deleted ? "1" : "0");
  AppendLineField(out, &first, row.previous_version_uuid);
  AppendLineU64Field(out, &first, row.previous_sequence);
  AppendEncodedCrudPairsFieldWithEncodedKeys(out, &first, values, encoded_keys);
  AppendLineField(out, &first, row.temporary_session_uuid);
}

void AppendTypedRowVersionStoreLine(
    std::string* out,
    const CrudRowVersionRecord& row,
    std::uint64_t event_sequence_override,
    const EngineRowValue& typed_row,
    std::span<const std::string> field_order,
    const std::vector<std::string>& encoded_keys) {
  if (out == nullptr) { return; }
  std::size_t encoded_value_bytes = 0;
  for (const auto& [_, typed] : typed_row.fields) {
    encoded_value_bytes += typed.isSqlNull() ? sizeof("<NULL>") - 1
                                             : typed.encoded_value.size();
  }
  const std::size_t fixed_size_floor =
      128 + row.table_uuid.size() + row.row_uuid.size() +
      row.version_uuid.size() + row.previous_version_uuid.size() +
      row.temporary_session_uuid.size();
  const std::size_t available =
      out->capacity() > out->size() ? out->capacity() - out->size() : 0;
  if (available < fixed_size_floor + kRowVersionStoreLineReserveSlackBytes) {
    ReserveAmortizedAppendCapacity(
        out,
        fixed_size_floor + encoded_value_bytes +
            field_order.size() * 8);
  }
  bool first = true;
  AppendLineField(out, &first, kRowStoreMagic);
  AppendLineField(out, &first, "ROW_VERSION");
  AppendLineU64Field(out, &first, row.creator_tx);
  AppendLineU64Field(out, &first, event_sequence_override);
  AppendLineField(out, &first, row.table_uuid);
  AppendLineField(out, &first, row.row_uuid);
  AppendLineField(out, &first, row.version_uuid);
  AppendLineField(out, &first, row.deleted ? "1" : "0");
  AppendLineField(out, &first, row.previous_version_uuid);
  AppendLineU64Field(out, &first, row.previous_sequence);
  AppendEncodedTypedFieldsFieldWithEncodedKeys(out,
                                               &first,
                                               typed_row,
                                               field_order,
                                               encoded_keys);
  AppendLineField(out, &first, row.temporary_session_uuid);
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

std::string BuildIndexEntryStoreLine(std::uint64_t creator_tx,
                                     std::uint64_t event_sequence,
                                     std::string_view index_uuid,
                                     std::string_view table_uuid,
                                     std::string_view column_name,
                                     std::string_view family,
                                     std::string_view entry_kind,
                                     std::string_view key,
                                     std::string_view payload,
                                     std::string_view row_uuid,
                                     std::string_view version_uuid) {
  const std::string creator_text = std::to_string(creator_tx);
  const std::string event_text = std::to_string(event_sequence);
  std::string line;
  line.reserve(96 + creator_text.size() + event_text.size() +
               index_uuid.size() + table_uuid.size() + column_name.size() +
               family.size() + entry_kind.size() +
               kLineHexFieldPrefix.size() + key.size() * 2 +
               kLineHexFieldPrefix.size() + payload.size() * 2 +
               row_uuid.size() + version_uuid.size());
  bool first = true;
  AppendLineField(&line, &first, kRowStoreMagic);
  AppendLineField(&line, &first, "INDEX_ENTRY");
  AppendLineField(&line, &first, creator_text);
  AppendLineField(&line, &first, event_text);
  AppendLineField(&line, &first, index_uuid);
  AppendLineField(&line, &first, table_uuid);
  AppendLineField(&line, &first, column_name);
  AppendLineField(&line, &first, family);
  AppendLineField(&line, &first, entry_kind);
  AppendLineSafeOrHexField(&line, &first, key);
  AppendLineSafeOrHexField(&line, &first, payload);
  AppendLineField(&line, &first, row_uuid);
  AppendLineField(&line, &first, version_uuid);
  return line;
}

void AppendIndexEntryStoreLine(std::string* out,
                               std::uint64_t creator_tx,
                               std::uint64_t event_sequence,
                               std::string_view index_uuid,
                               std::string_view table_uuid,
                               std::string_view column_name,
                               std::string_view family,
                               std::string_view entry_kind,
                               std::string_view key,
                               std::string_view payload,
                               std::string_view row_uuid,
                               std::string_view version_uuid) {
  if (out == nullptr) { return; }
  ReserveAmortizedAppendCapacity(out,
                                 128 + index_uuid.size() + table_uuid.size() +
                                     column_name.size() + family.size() +
                                     entry_kind.size() +
                                     kLineHexFieldPrefix.size() + key.size() * 2 +
                                     kLineHexFieldPrefix.size() + payload.size() * 2 +
                                     row_uuid.size() +
                                     version_uuid.size());
  bool first = true;
  AppendLineField(out, &first, kRowStoreMagic);
  AppendLineField(out, &first, "INDEX_ENTRY");
  AppendLineU64Field(out, &first, creator_tx);
  AppendLineU64Field(out, &first, event_sequence);
  AppendLineField(out, &first, index_uuid);
  AppendLineField(out, &first, table_uuid);
  AppendLineField(out, &first, column_name);
  AppendLineField(out, &first, family);
  AppendLineField(out, &first, entry_kind);
  AppendLineSafeOrHexField(out, &first, key);
  AppendLineSafeOrHexField(out, &first, payload);
  AppendLineField(out, &first, row_uuid);
  AppendLineField(out, &first, version_uuid);
  out->push_back('\n');
}

struct PreparedIndexEntryLine {
  std::string table_uuid;
  std::string index_uuid;
  std::string column_name;
  std::string family;
  std::string entry_kind;
  std::string key;
  std::string payload;
  std::string row_uuid;
  std::string version_uuid;
};

struct PreparedIndexAppendJob {
  bool ok = true;
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::vector<PreparedIndexEntryLine> entries;
  std::uint64_t sorted_batch_count = 0;
};

struct PreparedIndexLineBufferJob {
  bool ok = true;
  std::map<std::string, std::string> scoped_lines;
};

constexpr std::size_t kHotAppendRowLineReserveBytes = 384;
constexpr std::size_t kHotAppendIndexLineReserveBytes = 384;

bool PreparedIndexEntryLineLess(const PreparedIndexEntryLine& left,
                                const PreparedIndexEntryLine& right) {
  return std::tie(left.table_uuid,
                  left.index_uuid,
                  left.key,
                  left.row_uuid,
                  left.version_uuid) <
         std::tie(right.table_uuid,
                  right.index_uuid,
                  right.key,
                  right.row_uuid,
                  right.version_uuid);
}

void SortPreparedIndexEntryRangeIfNeeded(
    std::vector<PreparedIndexEntryLine>* entries,
    std::size_t first,
    std::uint64_t* sorted_batch_count) {
  if (entries == nullptr || sorted_batch_count == nullptr ||
      entries->size() <= first + 1) {
    return;
  }
  auto begin = entries->begin() + static_cast<std::ptrdiff_t>(first);
  auto end = entries->end();
  if (std::is_sorted(begin, end, PreparedIndexEntryLineLess)) {
    return;
  }
  ++(*sorted_batch_count);
  std::stable_sort(begin, end, PreparedIndexEntryLineLess);
}

bool BulkSortIndexMaterialAllowed(const CrudIndexRecord& index) {
  const std::string family =
      index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
  return family == kCrudIndexFamilyBtree ||
         family == "unique_btree" ||
         index.unique ||
         family == kCrudIndexFamilyExpression ||
         family == kCrudIndexFamilyPartial ||
         family == kCrudIndexFamilyCovering;
}

bool ExactIndexEntryInputLess(const MgaExactIndexEntryAppendBatch& batch,
                              const MgaExactIndexEntryInput& left,
                              const MgaExactIndexEntryInput& right) {
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  return std::tie(table_uuid,
                  batch.index.index_uuid,
                  left.encoded_key,
                  left.row_uuid,
                  left.version_uuid) <
         std::tie(table_uuid,
                  batch.index.index_uuid,
                  right.encoded_key,
                  right.row_uuid,
                  right.version_uuid);
}

bool ExactIndexBatchAlreadyInAppendOrder(
    const MgaExactIndexEntryAppendBatch& batch) {
  if (batch.entries.size() <= 1 || !BulkSortIndexMaterialAllowed(batch.index)) {
    return true;
  }
  return std::is_sorted(batch.entries.begin(),
                        batch.entries.end(),
                        [&](const auto& left, const auto& right) {
                          return ExactIndexEntryInputLess(batch, left, right);
                        });
}

void AddPreparedIndexAppendBatch(const MgaIndexEntryAppendBatch& batch,
                                 PreparedIndexAppendJob* job) {
  if (job == nullptr || batch.rows.empty()) { return; }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  const bool sort_allowed = BulkSortIndexMaterialAllowed(batch.index);
  const std::size_t before_batch = job->entries.size();
  job->entries.reserve(before_batch + batch.rows.size());
  for (const auto& row : batch.rows) {
    const auto keys = CrudIndexKeysForValues(batch.index, row.values);
    if (keys.empty()) { continue; }
    if (keys.size() > 1) {
      job->entries.reserve(job->entries.size() + keys.size());
    }
    const std::string payload =
        CrudFieldValue(row.values, batch.index.column_name);
    for (const auto& key : keys) {
      job->entries.push_back({table_uuid,
                              batch.index.index_uuid,
                              batch.index.column_name,
                              batch.index.family,
                              "exact",
                              key,
                              payload,
                              row.row_uuid,
                              row.version_uuid});
    }
  }
  if (sort_allowed && job->entries.size() > before_batch + 1) {
    SortPreparedIndexEntryRangeIfNeeded(&job->entries,
                                        before_batch,
                                        &job->sorted_batch_count);
  }
}

PreparedIndexAppendJob BuildPreparedIndexAppendJob(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  PreparedIndexAppendJob job;
  try {
    for (const auto& batch : batches) {
      AddPreparedIndexAppendBatch(batch, &job);
    }
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "index_materialization_failed");
  }
  return job;
}

PreparedIndexAppendJob BuildPreparedIndexAppendJob(
    const MgaIndexEntryAppendBatch& batch) {
  PreparedIndexAppendJob job;
  try {
    AddPreparedIndexAppendBatch(batch, &job);
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        std::string("index_materialization_failed:") + ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        "index_materialization_failed");
  }
  return job;
}

void AddPreparedExactIndexAppendBatch(const MgaExactIndexEntryAppendBatch& batch,
                                      PreparedIndexAppendJob* job) {
  if (job == nullptr || batch.entries.empty()) { return; }
  const std::string table_uuid =
      batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
  const bool sort_allowed = BulkSortIndexMaterialAllowed(batch.index);
  const std::size_t before_batch = job->entries.size();
  job->entries.reserve(before_batch + batch.entries.size());
  for (const auto& entry : batch.entries) {
    if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
        entry.version_uuid.empty()) {
      job->ok = false;
      job->diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                     "exact_index_entry_invalid");
      return;
    }
    job->entries.push_back({table_uuid,
                            batch.index.index_uuid,
                            batch.index.column_name,
                            batch.index.family,
                            "exact",
                            entry.encoded_key,
                            entry.payload_value,
                            entry.row_uuid,
                            entry.version_uuid});
  }
  if (sort_allowed && job->entries.size() > before_batch + 1) {
    SortPreparedIndexEntryRangeIfNeeded(&job->entries,
                                        before_batch,
                                        &job->sorted_batch_count);
  }
}

PreparedIndexAppendJob BuildPreparedExactIndexAppendJob(
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  PreparedIndexAppendJob job;
  try {
    for (const auto& batch : batches) {
      AddPreparedExactIndexAppendBatch(batch, &job);
      if (!job.ok) { return job; }
    }
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("exact_index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "exact_index_materialization_failed");
  }
  return job;
}

PreparedIndexAppendJob BuildPreparedExactIndexAppendJob(
    const MgaExactIndexEntryAppendBatch& batch) {
  PreparedIndexAppendJob job;
  try {
    AddPreparedExactIndexAppendBatch(batch, &job);
  } catch (const std::exception& ex) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  std::string("exact_index_materialization_failed:") +
                                                      ex.what());
  } catch (...) {
    job.ok = false;
    job.diagnostic = MakeInvalidRequestDiagnostic("mga.index_store",
                                                  "exact_index_materialization_failed");
  }
  return job;
}

PreparedIndexLineBufferJob BuildPreparedIndexLineBuffers(
    const EngineRequestContext& context,
    std::vector<PreparedIndexEntryLine> entries,
    std::uint64_t first_event_sequence) {
  PreparedIndexLineBufferJob job;
  if (entries.empty()) {
    return job;
  }
  const std::string single_table_uuid = entries.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(entries.begin()), entries.end(), [&](const auto& entry) {
        return entry.table_uuid == single_table_uuid;
      });
  if (single_table_batch) {
    const std::string scoped_path = ScopedIndexStorePath(context, single_table_uuid);
    std::string& scoped_buffer = job.scoped_lines[scoped_path];
    scoped_buffer.reserve(entries.size() * kHotAppendIndexLineReserveBytes);
    std::uint64_t event_sequence = first_event_sequence;
    for (const auto& entry : entries) {
      AppendIndexEntryStoreLine(&scoped_buffer,
                                context.local_transaction_id,
                                event_sequence++,
                                entry.index_uuid,
                                entry.table_uuid,
                                entry.column_name,
                                entry.family,
                                entry.entry_kind,
                                entry.key,
                                entry.payload,
                                entry.row_uuid,
                                entry.version_uuid);
    }
    return job;
  }
  std::map<std::string, std::size_t> entries_per_path;
  for (const auto& entry : entries) {
    entries_per_path[ScopedIndexStorePath(context, entry.table_uuid)] += 1;
  }
  for (const auto& [path, count] : entries_per_path) {
    job.scoped_lines[path].reserve(count * kHotAppendIndexLineReserveBytes);
  }
  std::uint64_t event_sequence = first_event_sequence;
  for (const auto& entry : entries) {
    std::string& scoped_buffer =
        job.scoped_lines[ScopedIndexStorePath(context, entry.table_uuid)];
    AppendIndexEntryStoreLine(&scoped_buffer,
                              context.local_transaction_id,
                              event_sequence++,
                              entry.index_uuid,
                              entry.table_uuid,
                              entry.column_name,
                              entry.family,
                              entry.entry_kind,
                              entry.key,
                              entry.payload,
                              entry.row_uuid,
                              entry.version_uuid);
  }
  return job;
}

std::uint64_t ParseU64(const std::string& text, std::uint64_t fallback = 0) {
  if (text.empty()) { return fallback; }
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

struct ScopedRelationSummaryDelta {
  std::uint64_t row_version_count = 0;
  std::uint64_t tombstone_count = 0;
  std::uint64_t update_count = 0;
  bool first_scoped_write = false;
};

ScopedRelationSummary RebuildScopedRelationSummaryFromRows(
    const std::string& scoped_row_path) {
  ScopedRelationSummary summary;
  if (!FileExistsAndNotEmpty(scoped_row_path)) {
    summary.trusted = true;
    return summary;
  }
  for (const auto& line : ReadLines(scoped_row_path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      summary.malformed = true;
      summary.trusted = false;
      return summary;
    }
    ++summary.row_version_count;
    if (fields[7] == "1") {
      ++summary.tombstone_count;
    }
    if (!fields[8].empty()) {
      ++summary.update_count;
    }
  }
  summary.trusted = true;
  return summary;
}

void MergeScopedRelationSummary(ScopedRelationSummary* target,
                                const ScopedRelationSummary& source) {
  if (target == nullptr) { return; }
  if (source.malformed || !source.trusted) {
    target->malformed = true;
    target->trusted = false;
    return;
  }
  target->row_version_count += source.row_version_count;
  target->tombstone_count += source.tombstone_count;
  target->update_count += source.update_count;
  target->trusted = true;
}

ScopedRelationSummary RebuildScopedRelationSummaryFromStores(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  ScopedRelationSummary summary;
  summary.trusted = true;
  MergeScopedRelationSummary(
      &summary,
      RebuildScopedRelationSummaryFromRows(ScopedRowStorePath(context,
                                                              table_uuid)));
  ScopedRelationSummary binary_summary;
  std::vector<CrudRowVersionRecord> ignored_rows;
  if (!DecodeScopedRowBinaryStore(ScopedRowBinaryStorePath(context,
                                                          table_uuid),
                                  &ignored_rows,
                                  &binary_summary)) {
    binary_summary.malformed = true;
    binary_summary.trusted = false;
  }
  MergeScopedRelationSummary(&summary, binary_summary);
  return summary;
}

bool ScopedRelationAnyRowStoreExists(const EngineRequestContext& context,
                                     const std::string& table_uuid) {
  return FileExistsAndNotEmpty(ScopedRowStorePath(context, table_uuid)) ||
         FileExistsAndNotEmpty(ScopedRowBinaryStorePath(context, table_uuid));
}

ScopedRelationSummary LoadScopedRelationSummary(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  const std::string summary_path = ScopedSummaryStorePath(context, table_uuid);
  const std::string row_path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_row_path = ScopedRowBinaryStorePath(context,
                                                              table_uuid);
  if (!FileExistsAndNotEmpty(summary_path)) {
    if (!FileExistsAndNotEmpty(row_path) &&
        !FileExistsAndNotEmpty(binary_row_path)) {
      ScopedRelationSummary empty;
      empty.trusted = true;
      return empty;
    }
    return RebuildScopedRelationSummaryFromStores(context, table_uuid);
  }

  ScopedRelationSummary summary;
  for (const auto& line : ReadLines(summary_path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 8 || fields[0] != "SBMGASUM1" ||
        fields[1] != "RELATION_SCOPE") {
      summary.malformed = true;
      summary.trusted = false;
      return summary;
    }
    for (std::size_t index = 2; index + 1 < fields.size(); index += 2) {
      if (fields[index] == "row_versions") {
        summary.row_version_count = ParseU64(fields[index + 1]);
      } else if (fields[index] == "tombstones") {
        summary.tombstone_count = ParseU64(fields[index + 1]);
      } else if (fields[index] == "updates") {
        summary.update_count = ParseU64(fields[index + 1]);
      }
    }
    summary.trusted = true;
  }
  if (!summary.trusted &&
      (FileExistsAndNotEmpty(row_path) ||
       FileExistsAndNotEmpty(binary_row_path))) {
    return RebuildScopedRelationSummaryFromStores(context, table_uuid);
  }
  summary.trusted = true;
  return summary;
}

bool WriteScopedRelationSummary(const EngineRequestContext& context,
                                const std::string& table_uuid,
                                const ScopedRelationSummary& summary) {
  std::error_code ignored;
  std::filesystem::create_directories(ScopedRelationStoreRoot(context),
                                      ignored);
  const std::string path = ScopedSummaryStorePath(context, table_uuid);
  const std::string tmp_path = path + ".tmp." +
      std::to_string(context.local_transaction_id) + "." +
      std::to_string(summary.row_version_count);
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) { return false; }
    out << JoinLine({"SBMGASUM1",
                     "RELATION_SCOPE",
                     "row_versions",
                     std::to_string(summary.row_version_count),
                     "tombstones",
                     std::to_string(summary.tombstone_count),
                     "updates",
                     std::to_string(summary.update_count)})
        << '\n';
    out.flush();
    if (!out) { return false; }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  return !rename_error;
}

bool UpdateScopedRelationSummaries(
    const EngineRequestContext& context,
    const std::map<std::string, ScopedRelationSummaryDelta>& deltas) {
  if (deltas.empty()) {
    return true;
  }
  const std::lock_guard<std::mutex> guard(ScopedRelationSummaryMutex());
  for (const auto& [table_uuid, delta] : deltas) {
    if (table_uuid.empty() || delta.row_version_count == 0) {
      continue;
    }
    const std::string summary_path = ScopedSummaryStorePath(context,
                                                            table_uuid);
    if (FileExistsAndNotEmpty(summary_path)) {
      ScopedRelationSummary summary = LoadScopedRelationSummary(context,
                                                                table_uuid);
      if (summary.malformed) {
        return false;
      }
      summary.row_version_count += delta.row_version_count;
      summary.tombstone_count += delta.tombstone_count;
      summary.update_count += delta.update_count;
      summary.trusted = true;
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    } else if (delta.first_scoped_write) {
      ScopedRelationSummary summary;
      summary.row_version_count = delta.row_version_count;
      summary.tombstone_count = delta.tombstone_count;
      summary.update_count = delta.update_count;
      summary.trusted = true;
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    } else {
      ScopedRelationSummary summary =
          RebuildScopedRelationSummaryFromStores(context, table_uuid);
      if (!summary.trusted || summary.malformed) {
        return false;
      }
      if (!WriteScopedRelationSummary(context, table_uuid, summary)) {
        return false;
      }
    }
  }
  return true;
}

std::mutex& EventSequenceCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::uint64_t>& EventSequenceCache() {
  static std::map<std::string, std::uint64_t> cache;
  return cache;
}

std::string EventSequenceStreamKey(const std::string& stream_kind,
                                   const std::string& stream_path) {
  return stream_kind + "\n" + stream_path;
}

struct DurableEventSequenceState {
  bool found = false;
  std::uint64_t next = 0;
};

DurableEventSequenceState LoadDurableEventSequenceState(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path) {
  DurableEventSequenceState state;
  const std::string encoded_stream_path = EncodeCrudText(stream_path);
  for (const auto& line : ReadLines(EventSequenceAllocatorStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 7 ||
        fields[0] != kEventSequenceAllocatorMagic ||
        fields[1] != "RANGE" ||
        fields[2] != stream_kind ||
        fields[3] != encoded_stream_path) {
      continue;
    }
    const std::uint64_t next = ParseU64(fields[6]);
    if (next != 0) {
      state.found = true;
      state.next = next;
    }
  }
  return state;
}

std::uint64_t NextReservedEventSequence(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t fallback_next) {
  std::uint64_t next = fallback_next == 0 ? 1 : fallback_next;
  {
    const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
    const auto cache_it =
        EventSequenceCache().find(EventSequenceStreamKey(stream_kind, stream_path));
    if (cache_it != EventSequenceCache().end() && cache_it->second != 0) {
      next = std::max(next, cache_it->second);
    }
  }
  const auto durable = LoadDurableEventSequenceState(context, stream_kind, stream_path);
  if (durable.found && durable.next != 0) {
    next = std::max(next, durable.next);
  }
  return next == 0 ? 1 : next;
}

MgaEventSequenceRangeReservation RefuseEventSequenceReservation(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    const std::string& reason) {
  MgaEventSequenceRangeReservation reservation;
  reservation.allocator_path = EventSequenceAllocatorStorePath(context);
  reservation.stream_kind = stream_kind;
  reservation.stream_path = stream_path;
  reservation.diagnostic = MakeInvalidRequestDiagnostic("mga.event_sequence_allocator",
                                                        reason);
  return reservation;
}

template <typename Loader>
MgaEventSequenceRangeReservation ReserveEventSequenceRange(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t count,
    Loader loader,
    std::vector<std::string>* deferred_allocator_lines = nullptr) {
  if (context.database_path.empty()) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "database_path_required");
  }
  if (stream_kind.empty() || stream_path.empty()) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "stream_identity_required");
  }
  const std::uint64_t normalized_count = count == 0 ? 1 : count;
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  const std::string cache_key = EventSequenceStreamKey(stream_kind, stream_path);
  auto cache_it = EventSequenceCache().find(cache_key);
  std::uint64_t next_sequence = 0;
  bool bootstrapped = false;
  std::string route = "process_cache_after_durable_reservation";
  if (cache_it != EventSequenceCache().end() && cache_it->second != 0) {
    next_sequence = cache_it->second;
  } else {
    const auto durable = LoadDurableEventSequenceState(context, stream_kind, stream_path);
    if (durable.found) {
      next_sequence = durable.next;
      route = "durable_allocator_state";
    } else {
      next_sequence = loader();
      bootstrapped = true;
      route = "bootstrap_store_scan";
    }
  }
  if (next_sequence == 0) {
    next_sequence = 1;
  }
  if (normalized_count > std::numeric_limits<std::uint64_t>::max() - next_sequence) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "event_sequence_range_overflow");
  }
  const std::uint64_t first = next_sequence;
  const std::uint64_t next = next_sequence + normalized_count;
  const std::string allocator_path = EventSequenceAllocatorStorePath(context);
  const std::string line = JoinLine({kEventSequenceAllocatorMagic,
                                     "RANGE",
                                     stream_kind,
                                     EncodeCrudText(stream_path),
                                     std::to_string(first),
                                     std::to_string(normalized_count),
                                     std::to_string(next),
                                     route,
                                     bootstrapped ? "1" : "0"});
  if (deferred_allocator_lines != nullptr) {
    deferred_allocator_lines->push_back(line);
  } else if (!AppendLine(allocator_path, line)) {
    return RefuseEventSequenceReservation(context,
                                          stream_kind,
                                          stream_path,
                                          "durable_allocator_append_failed");
  }
  EventSequenceCache()[cache_key] = next;

  MgaEventSequenceRangeReservation reservation;
  reservation.ok = true;
  reservation.diagnostic = OkDiagnostic();
  reservation.allocator_path = allocator_path;
  reservation.stream_kind = stream_kind;
  reservation.stream_path = stream_path;
  reservation.first = first;
  reservation.count = normalized_count;
  reservation.next = next;
  reservation.bootstrapped_from_store = bootstrapped;
  return reservation;
}

std::uint64_t ScanNextRowEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic && fields[1] == "ROW_VERSION") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextRowEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(context,
                                   "row_versions",
                                   RowStorePath(context),
                                   ScanNextRowEventSequence(context));
}

std::uint64_t ScanNextIndexEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(IndexStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic && fields[1] == "INDEX_ENTRY") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextIndexEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(context,
                                   "index_entries",
                                   IndexStorePath(context),
                                   ScanNextIndexEventSequence(context));
}

std::uint64_t ScanNextMetadataEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        (fields[1] == "TABLE_METADATA" || fields[1] == "INDEX_METADATA" ||
         fields[1] == "CONSTRAINT_MUTATION_BATCH")) {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextMetadataEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(context,
                                   "relation_metadata",
                                   MetadataStorePath(context),
                                   ScanNextMetadataEventSequence(context));
}

std::uint64_t ChecksumText(const std::string& value) {
  std::uint64_t checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<std::uint64_t>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
}

void AppendCanonicalBatchField(std::string* out,
                               std::string_view key,
                               std::string_view value) {
  if (out == nullptr) return;
  out->append(std::to_string(key.size()));
  out->push_back(':');
  out->append(key);
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::string CanonicalConstraintMutationBatchPayload(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  std::string payload;
  auto field = [&](std::string_view key, std::string_view value) {
    AppendCanonicalBatchField(&payload, key, value);
  };
  field("format_version", batch.format_version);
  field("seal_state", "sealed");
  field("creator_local_transaction_id",
        std::to_string(creator_local_transaction_id));
  // MGA savepoint rollback and metadata ordering both depend on this value;
  // bind it into the seal so a batch cannot be replayed at another event.
  field("metadata_event_sequence", std::to_string(metadata_event_sequence));
  field("batch_uuid", batch.batch_uuid);
  field("mutation_count", std::to_string(batch.mutation_count));
  field("database_uuid", batch.database_uuid);
  field("constraint_uuid", batch.constraint_uuid);
  field("owner_table_uuid", batch.owner_table_uuid);
  field("child_schema_uuid", batch.child_schema_uuid);
  field("child_relation_descriptor_uuid",
        batch.child_relation_descriptor_uuid);
  field("child_relation_descriptor_generation",
        std::to_string(batch.child_relation_descriptor_generation));
  field("child_column_uuid", batch.child_column_uuid);
  field("parent_table_uuid", batch.parent_table_uuid);
  field("parent_schema_uuid", batch.parent_schema_uuid);
  field("parent_relation_descriptor_uuid",
        batch.parent_relation_descriptor_uuid);
  field("parent_relation_descriptor_generation",
        std::to_string(batch.parent_relation_descriptor_generation));
  field("parent_column_uuid", batch.parent_column_uuid);
  field("parent_candidate_key_constraint_uuid",
        batch.parent_candidate_key_constraint_uuid);
  field("key_descriptor_uuid", batch.key_descriptor_uuid);
  field("support_uuid", batch.support_uuid);
  field("support_family", batch.support_family);
  field("support_policy", batch.support_policy);
  field("match_policy", batch.match_policy);
  field("on_update_action", batch.on_update_action);
  field("on_delete_action", batch.on_delete_action);
  field("enforcement_timing", batch.enforcement_timing);
  field("constraint_metadata_generation",
        std::to_string(batch.constraint_metadata_generation));
  field("base_table_event_sequence",
        std::to_string(batch.base_table_event_sequence));
  field("parent_base_table_event_sequence",
        std::to_string(batch.parent_base_table_event_sequence));
  field("constraint_name", batch.constraint_name);
  field("constraint_kind", batch.constraint_kind);
  field("canonical_constraint_envelope",
        batch.canonical_constraint_envelope);
  field("updated_table_uuid", batch.updated_table.table_uuid);
  field("updated_table_default_name", batch.updated_table.default_name);
  field("updated_table_columns", EncodeCrudPairs(batch.updated_table.columns));
  field("updated_table_temporary",
        batch.updated_table.temporary ? "true" : "false");
  field("updated_table_temporary_scope", batch.updated_table.temporary_scope);
  field("updated_table_temporary_session_uuid",
        batch.updated_table.temporary_session_uuid);
  field("updated_table_on_commit_action", batch.updated_table.on_commit_action);
  return payload;
}

std::string ConstraintMutationBatchSha256(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  const std::string payload = CanonicalConstraintMutationBatchPayload(
      batch, creator_local_transaction_id, metadata_event_sequence);
  const auto* bytes = reinterpret_cast<
      const scratchbird::core::platform::byte*>(payload.data());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes, payload.size());
  if (!digest.ok() ||
      digest.digest_bytes != scratchbird::core::hash::kSha256DigestBytes) {
    return {};
  }
  return "sha256:" + scratchbird::core::hash::HexLower(digest.digest);
}

bool ValidConstraintBatchUuid(
    std::string_view value,
    scratchbird::core::platform::UuidKind kind) {
  if (value.empty()) return false;
  return scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
             kind, std::string(value))
      .ok();
}

namespace constraint_batch_field {
inline constexpr std::size_t kMagic = 0;
inline constexpr std::size_t kRecordKind = 1;
inline constexpr std::size_t kCreatorTx = 2;
inline constexpr std::size_t kEventSequence = 3;
inline constexpr std::size_t kFormatVersion = 4;
inline constexpr std::size_t kBatchUuid = 5;
inline constexpr std::size_t kSealState = 6;
inline constexpr std::size_t kBatchHash = 7;
inline constexpr std::size_t kMutationCount = 8;
inline constexpr std::size_t kDatabaseUuid = 9;
inline constexpr std::size_t kConstraintUuid = 10;
inline constexpr std::size_t kOwnerTableUuid = 11;
inline constexpr std::size_t kChildSchemaUuid = 12;
inline constexpr std::size_t kChildDescriptorUuid = 13;
inline constexpr std::size_t kChildDescriptorGeneration = 14;
inline constexpr std::size_t kChildColumnUuid = 15;
inline constexpr std::size_t kParentTableUuid = 16;
inline constexpr std::size_t kParentSchemaUuid = 17;
inline constexpr std::size_t kParentDescriptorUuid = 18;
inline constexpr std::size_t kParentDescriptorGeneration = 19;
inline constexpr std::size_t kParentColumnUuid = 20;
inline constexpr std::size_t kParentCandidateConstraintUuid = 21;
inline constexpr std::size_t kReferencedKeyDescriptorUuid = 22;
inline constexpr std::size_t kSupportUuid = 23;
inline constexpr std::size_t kSupportFamily = 24;
inline constexpr std::size_t kSupportPolicy = 25;
inline constexpr std::size_t kMatchPolicy = 26;
inline constexpr std::size_t kOnUpdate = 27;
inline constexpr std::size_t kOnDelete = 28;
inline constexpr std::size_t kEnforcementTiming = 29;
inline constexpr std::size_t kConstraintMetadataGeneration = 30;
inline constexpr std::size_t kBaseTableEventSequence = 31;
inline constexpr std::size_t kParentBaseTableEventSequence = 32;
inline constexpr std::size_t kConstraintName = 33;
inline constexpr std::size_t kConstraintKind = 34;
inline constexpr std::size_t kCanonicalEnvelope = 35;
inline constexpr std::size_t kTableUuid = 36;
inline constexpr std::size_t kTableDefaultName = 37;
inline constexpr std::size_t kTableColumns = 38;
inline constexpr std::size_t kTableTemporary = 39;
inline constexpr std::size_t kTableTemporaryScope = 40;
inline constexpr std::size_t kTableTemporarySessionUuid = 41;
inline constexpr std::size_t kTableOnCommitAction = 42;
inline constexpr std::size_t kFieldCount = 43;
}  // namespace constraint_batch_field

std::vector<std::string> ConstraintMutationBatchLineFields(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_tx,
    std::uint64_t event_sequence) {
  const CrudTableRecord& table = batch.updated_table;
  std::vector<std::string> fields{
      kRowStoreMagic,
      "CONSTRAINT_MUTATION_BATCH",
      std::to_string(creator_tx),
      std::to_string(event_sequence),
      batch.format_version,
      batch.batch_uuid,
      "sealed",
      batch.batch_hash,
      std::to_string(batch.mutation_count),
      batch.database_uuid,
      batch.constraint_uuid,
      batch.owner_table_uuid,
      batch.child_schema_uuid,
      batch.child_relation_descriptor_uuid,
      std::to_string(batch.child_relation_descriptor_generation),
      batch.child_column_uuid,
      batch.parent_table_uuid,
      batch.parent_schema_uuid,
      batch.parent_relation_descriptor_uuid,
      std::to_string(batch.parent_relation_descriptor_generation),
      batch.parent_column_uuid,
      batch.parent_candidate_key_constraint_uuid,
      batch.key_descriptor_uuid,
      batch.support_uuid,
      batch.support_family,
      batch.support_policy,
      batch.match_policy,
      batch.on_update_action,
      batch.on_delete_action,
      batch.enforcement_timing,
      std::to_string(batch.constraint_metadata_generation),
      std::to_string(batch.base_table_event_sequence),
      std::to_string(batch.parent_base_table_event_sequence),
      EncodeCrudText(batch.constraint_name),
      batch.constraint_kind,
      EncodeCrudText(batch.canonical_constraint_envelope),
      table.table_uuid,
      EncodeCrudText(table.default_name),
      EncodeCrudPairs(table.columns),
      table.temporary ? "1" : "0",
      table.temporary_scope,
      table.temporary_session_uuid,
      table.on_commit_action};
  return fields;
}

idx::SecondaryIndexDeltaLedgerLimits DefaultSecondaryIndexDeltaLedgerLimits() {
  return {};
}

EngineApiDiagnostic DiagnosticFromSecondaryIndexDeltaLedger(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code
                                                                    : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key
                                                                : diagnostic.message_key,
                                 detail,
                                 true);
}

bool IsUniqueMgaIndex(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") !=
             index.key_envelopes.end();
}

EngineApiDiagnostic ParseLedgerTypedUuid(const std::string& text,
                                         scratchbird::core::platform::UuidKind kind,
                                         idx::TypedUuid* out) {
  if (text.empty() || out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "typed_uuid_required");
  }
  const auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(kind, text);
  if (!parsed.ok()) {
    return DiagnosticFromSecondaryIndexDeltaLedger(
        parsed.diagnostic,
        "SB-MGA-SECONDARY-DELTA-UUID-INVALID",
        "mga.secondary_index_delta_ledger.invalid_uuid");
  }
  *out = parsed.value;
  return OkDiagnostic();
}

std::string MakeSecondaryIndexDeltaKeyPayload(
    const CrudIndexRecord& index,
    const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& values) {
  return EncodeCrudPairs({{"key", key},
                          {"payload", CrudFieldValue(values, index.column_name)},
                          {"family", index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                                                          : index.family}});
}

std::string MakeSecondaryIndexDeltaCleanupHorizonToken(const EngineRequestContext& context) {
  return "mga_cleanup_horizon:visible_through=" +
         std::to_string(context.snapshot_visible_through_local_transaction_id) +
         ":local_tx=" + std::to_string(context.local_transaction_id);
}

std::string MakeSecondaryIndexDeltaEvidenceReference(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaLedgerEntryInput& input,
    const std::string& key) {
  const std::string request_id = context.request_id.empty() ? "request_uuid_unset"
                                                           : context.request_id;
  return "engine.dml.secondary_index_delta:" + request_id +
         ":index=" + input.index.index_uuid +
         ":row=" + input.row_uuid +
         ":version=" + input.version_uuid +
         ":key_hash=" + std::to_string(ChecksumText(key));
}

std::uint64_t MaxCommittedLocalTransactionId(const CrudState& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

std::uint64_t SnapshotVisibleThroughForOverlay(const CrudState& state,
                                               const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : context.transaction_isolation_level;
  if ((isolation == "snapshot" || isolation == "repeatable_read" ||
       isolation == "serializable")) {
    return context.snapshot_visible_through_local_transaction_id;
  }
  return MaxCommittedLocalTransactionId(state);
}

std::string DeltaPayloadField(const std::string& key_payload,
                              const std::string& field) {
  for (const auto& [key, value] : DecodeCrudPairs(key_payload)) {
    if (key == field) {
      return value;
    }
  }
  return {};
}

int CompareOverlayScalar(const std::string& left, const std::string& right) {
  try {
    std::size_t left_end = 0;
    std::size_t right_end = 0;
    const auto left_number = std::stoll(left, &left_end);
    const auto right_number = std::stoll(right, &right_end);
    if (left_end == left.size() && right_end == right.size()) {
      if (left_number < right_number) { return -1; }
      if (left_number > right_number) { return 1; }
      return 0;
    }
  } catch (...) {
  }
  if (left < right) { return -1; }
  if (left > right) { return 1; }
  return 0;
}

bool OverlayPredicateSupported(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_range";
}

bool OverlayEntryMatchesPredicate(const idx::SecondaryIndexOverlayEntry& entry,
                                  const EnginePredicateEnvelope& predicate) {
  const std::string key = DeltaPayloadField(entry.key_payload, "key");
  if (predicate.predicate_kind == "column_equals") {
    return !predicate.bound_values.empty() &&
           key == predicate.bound_values.front().encoded_value;
  }
  if (predicate.predicate_kind == "column_in_list") {
    for (const auto& bound : predicate.bound_values) {
      if (key == bound.encoded_value) { return true; }
    }
    return false;
  }
  if (predicate.predicate_kind == "column_range") {
    const bool lower_ok = predicate.bound_values.empty() ||
        CompareOverlayScalar(key, predicate.bound_values[0].encoded_value) >= 0;
    const bool upper_ok = predicate.bound_values.size() < 2 ||
        CompareOverlayScalar(key, predicate.bound_values[1].encoded_value) <= 0;
    return lower_ok && upper_ok;
  }
  return false;
}

bool LedgerRecordRelevantToIndex(const idx::SecondaryIndexDeltaLedgerRecord& record,
                                 const CrudIndexRecord& index,
                                 const std::string& table_uuid) {
  return scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value) ==
             index.index_uuid &&
         scratchbird::core::uuid::UuidToString(record.delta.table_uuid.value) ==
             table_uuid;
}

EngineApiDiagnostic OverlayLookupDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty() ? fallback_code
                                                                    : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty() ? fallback_key
                                                                : diagnostic.message_key,
                                 detail,
                                 true);
}

std::optional<CrudIndexRecord> SelectCrudIndexForPredicate(const CrudState& state,
                                                           const std::string& table_uuid,
                                                           const EnginePredicateEnvelope& predicate,
                                                           std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (CrudIndexSupportsPredicate(index, predicate)) {
      return index;
    }
  }
  return std::nullopt;
}

idx::SecondaryIndexKind SecondaryIndexKindForCrudIndex(const CrudIndexRecord& index) {
  return IsUniqueMgaIndex(index) ? idx::SecondaryIndexKind::unique
                                : idx::SecondaryIndexKind::non_unique;
}

EngineApiDiagnostic BaseEntryForOverlay(const CrudIndexEntryRecord& entry,
                                        idx::SecondaryIndexBaseEntry* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_overlay",
                                        "base_entry_required");
  }
  idx::SecondaryIndexBaseEntry base;
  auto diagnostic = ParseLedgerTypedUuid(entry.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &base.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &base.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &base.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(entry.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &base.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  base.key_payload = EncodeCrudPairs({{"key", entry.key_value},
                                      {"payload", entry.payload_value},
                                      {"family", entry.family}});
  base.committed_local_transaction_id = 0;
  *out = std::move(base);
  return OkDiagnostic();
}

MgaIndexedRowsLookupResult RefuseIndexedLookup(std::string detail,
                                               EngineApiDiagnostic diagnostic = {}) {
  MgaIndexedRowsLookupResult result;
  result.ok = false;
  result.index_refused = true;
  result.diagnostic = diagnostic.error ? std::move(diagnostic)
                                       : MakeInvalidRequestDiagnostic(
                                             "mga.secondary_index_delta_overlay",
                                             std::move(detail));
  result.evidence.push_back({"mga_secondary_index_delta_overlay_refused_code",
                             result.diagnostic.code});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_refused",
                             result.diagnostic.detail});
  return result;
}

EngineApiDiagnostic Dpc024MergeDiagnostic(const std::string& code,
                                          const std::string& message_key,
                                          const std::string& detail) {
  return MakeEngineApiDiagnostic(code, message_key, detail, true);
}

void AddMergeEvidence(std::vector<EngineEvidenceReference>* evidence,
                      const std::string& kind,
                      const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

EngineApiDiagnostic Dpc025RecoveryDiagnostic(const std::string& code,
                                             const std::string& message_key,
                                             const std::string& detail) {
  return MakeEngineApiDiagnostic(code, message_key, detail, true);
}

void AddRecoveryEvidence(std::vector<EngineEvidenceReference>* evidence,
                         const std::string& kind,
                         const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

EngineApiDiagnostic Dpc033CleanupDiagnostic(const std::string& code,
                                            const std::string& message_key,
                                            const std::string& detail,
                                            bool error = true) {
  return MakeEngineApiDiagnostic(code, message_key, detail, error);
}

void AddIndexGarbageCleanupEvidence(std::vector<EngineEvidenceReference>* evidence,
                                    const std::string& kind,
                                    const std::string& value) {
  if (evidence == nullptr) { return; }
  evidence->push_back({kind, value});
}

void AddEventSequenceReservationEvidence(
    std::vector<EngineEvidenceReference>* evidence,
    const MgaEventSequenceRangeReservation& reservation) {
  if (evidence == nullptr || !reservation.ok) { return; }
  evidence->push_back({"mga_event_sequence_allocator_path",
                       reservation.allocator_path});
  evidence->push_back({"mga_event_sequence_allocator_stream",
                       reservation.stream_kind});
  evidence->push_back({"mga_event_sequence_allocator_first",
                       std::to_string(reservation.first)});
  evidence->push_back({"mga_event_sequence_allocator_count",
                       std::to_string(reservation.count)});
  evidence->push_back({"mga_event_sequence_allocator_next",
                       std::to_string(reservation.next)});
  evidence->push_back({"mga_event_sequence_allocator_bootstrap",
                       reservation.bootstrapped_from_store ? "true" : "false"});
}

std::string Dpc033DiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.remediation_hint.empty()) {
    return diagnostic.remediation_hint;
  }
  std::string detail;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) {
      detail += ";";
    }
    detail += argument.key + "=" + argument.value;
  }
  return detail;
}

bool IsDpc025CommittedTerminal(TransactionState state) {
  return state == TransactionState::committed ||
         state == TransactionState::archived;
}

bool IsDpc025RolledBackTerminal(TransactionState state) {
  return state == TransactionState::rolled_back ||
         state == TransactionState::failed_terminal;
}

bool Dpc025DeltaRequiresPublishedBase(idx::SecondaryIndexDeltaKind kind) {
  return kind == idx::SecondaryIndexDeltaKind::insert ||
         kind == idx::SecondaryIndexDeltaKind::update_after;
}

bool Dpc025PublishedBaseContainsRecord(const CrudState& state,
                                       const CrudIndexRecord& index,
                                       const std::string& table_uuid,
                                       const idx::SecondaryIndexDeltaLedgerRecord& record,
                                       EngineApiDiagnostic* diagnostic) {
  const std::string row_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.row_uuid.value);
  const std::string version_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.version_uuid.value);
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != index.index_uuid ||
        entry.table_uuid != table_uuid ||
        entry.row_uuid != row_uuid ||
        entry.version_uuid != version_uuid ||
        !CrudCreatorVisible(state, entry.creator_tx, entry.event_sequence, 0)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto converted = BaseEntryForOverlay(entry, &base);
    if (converted.error) {
      if (diagnostic != nullptr) { *diagnostic = converted; }
      return false;
    }
    if (base.key_payload == record.delta.key_payload) {
      return true;
    }
  }
  if (diagnostic != nullptr) {
    *diagnostic = Dpc025RecoveryDiagnostic(
        "secondary_index_delta_recovery_base_entry_missing",
        "mga.secondary_index_delta_recovery.base_entry_missing",
        "merged_cleaned ledger record has no matching committed base index entry");
  }
  return false;
}

EngineApiDiagnostic Dpc033TableSnapshotEntryForCleanup(
    const CrudIndexRecord& index,
    const CrudRowVersionRecord& row,
    const std::string& key,
    idx::SecondaryIndexTableSnapshotEntry* out) {
  if (out == nullptr) {
    return Dpc033CleanupDiagnostic("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                                   "mga.secondary_index_garbage_cleanup.snapshot_entry_required",
                                   "table snapshot entry output is required");
  }
  idx::SecondaryIndexTableSnapshotEntry entry;
  auto diagnostic = ParseLedgerTypedUuid(index.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &entry.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(index.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &entry.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(row.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &entry.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(row.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &entry.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  entry.key_payload = MakeSecondaryIndexDeltaKeyPayload(index, key, row.values);
  entry.deleted = row.deleted;
  *out = std::move(entry);
  return OkDiagnostic();
}

std::optional<CrudIndexRecord> FindVisibleCrudIndexByUuid(const CrudState& state,
                                                          const std::string& table_uuid,
                                                          const std::string& index_uuid,
                                                          std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (index.index_uuid == index_uuid) { return index; }
  }
  return std::nullopt;
}

bool LedgerRecordBelongsToUniqueIndex(const idx::SecondaryIndexDeltaLedgerRecord& record,
                                      const CrudState& state) {
  const std::string index_uuid =
      scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value);
  for (const auto& index : state.indexes) {
    if (index.index_uuid == index_uuid && IsUniqueMgaIndex(index)) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic CrudIndexEntryForMergedBase(const CrudIndexRecord& index,
                                                const idx::SecondaryIndexBaseEntry& base,
                                                CrudIndexEntryRecord* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_merge",
                                        "index_entry_required");
  }
  CrudIndexEntryRecord entry;
  entry.creator_tx = base.committed_local_transaction_id;
  entry.index_uuid = scratchbird::core::uuid::UuidToString(base.index_uuid.value);
  entry.table_uuid = scratchbird::core::uuid::UuidToString(base.table_uuid.value);
  entry.column_name = index.column_name;
  entry.family = DeltaPayloadField(base.key_payload, "family");
  if (entry.family.empty()) {
    entry.family = index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                                        : index.family;
  }
  entry.entry_kind = "exact";
  entry.key_value = DeltaPayloadField(base.key_payload, "key");
  entry.payload_value = DeltaPayloadField(base.key_payload, "payload");
  entry.row_uuid = scratchbird::core::uuid::UuidToString(base.row_uuid.value);
  entry.version_uuid = scratchbird::core::uuid::UuidToString(base.version_uuid.value);
  if (entry.index_uuid.empty() || entry.table_uuid.empty() || entry.row_uuid.empty() ||
      entry.version_uuid.empty() || entry.key_value.empty()) {
    return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                 "mga.secondary_index_delta_merge.corrupt_base_entry_refused",
                                 "merged base index entry lost required identity or key payload");
  }
  *out = std::move(entry);
  return OkDiagnostic();
}

EngineApiDiagnostic RewriteMgaIndexEntriesForMergedIndex(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudIndexRecord& index,
    const std::string& table_uuid,
    const std::vector<idx::SecondaryIndexBaseEntry>& base_entries) {
  const std::string path = IndexStorePath(context);
  const auto existing_lines = ReadLines(path);
  std::vector<std::string> output_lines;
  output_lines.reserve(existing_lines.size() + base_entries.size());
  std::uint64_t max_sequence = 0;
  for (const auto& line : existing_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 13 && fields[0] == kRowStoreMagic &&
        fields[1] == "INDEX_ENTRY") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
      if (fields[4] == index.index_uuid && fields[5] == table_uuid) {
        const std::uint64_t creator_tx = ParseU64(fields[2]);
        const auto tx = state.transactions.find(creator_tx);
        const bool transaction_still_live =
            tx != state.transactions.end() &&
            (tx->second == "active" || tx->second == "prepared");
        if (!CrudCreatorVisible(state, creator_tx, ParseU64(fields[3]), 0) &&
            transaction_still_live) {
          output_lines.push_back(line);
        }
        continue;
      }
    }
    output_lines.push_back(line);
  }

  std::uint64_t event_sequence = max_sequence + 1;
  for (const auto& base : base_entries) {
    if (scratchbird::core::uuid::UuidToString(base.index_uuid.value) != index.index_uuid ||
        scratchbird::core::uuid::UuidToString(base.table_uuid.value) != table_uuid ||
        base.deleted) {
      continue;
    }
    CrudIndexEntryRecord entry;
    const auto converted = CrudIndexEntryForMergedBase(index, base, &entry);
    if (converted.error) { return converted; }
    entry.event_sequence = event_sequence++;
    entry.sequence = entry.event_sequence;
    output_lines.push_back(JoinLine({kRowStoreMagic,
                                     "INDEX_ENTRY",
                                     std::to_string(entry.creator_tx),
                                     std::to_string(entry.event_sequence),
                                     entry.index_uuid,
                                     entry.table_uuid,
                                     entry.column_name,
                                     entry.family,
                                     entry.entry_kind,
                                     entry.key_value,
                                     entry.payload_value,
                                     entry.row_uuid,
                                     entry.version_uuid}));
  }

  const std::string tmp_path = path + ".tmp.merge." +
      std::to_string(context.local_transaction_id) + "." +
      std::to_string(output_lines.size());
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                   "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                   "temporary index-entry sidecar could not be opened");
    }
    for (const auto& line : output_lines) {
      out << line << '\n';
    }
    out.flush();
    if (!out) {
      return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                   "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                   "temporary index-entry sidecar could not be written");
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  if (rename_error) {
    return Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                 "mga.secondary_index_delta_merge.index_store_rewrite_failed",
                                 "index-entry sidecar replacement failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic BuildSecondaryIndexDeltaLedgerRecord(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaLedgerEntryInput& input,
    const std::string& key,
    idx::SecondaryIndexDeltaLedgerRecord* out) {
  if (out == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "ledger_record_required");
  }
  if (IsUniqueMgaIndex(input.index)) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "unique_index_delta_refused");
  }
  idx::SecondaryIndexDeltaLedgerRecord record;
  auto diagnostic = ParseLedgerTypedUuid(GenerateCrudEngineUuid("object"),
                                         scratchbird::core::platform::UuidKind::object,
                                         &record.delta.delta_id);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.index.index_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &record.delta.index_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.table_uuid.empty() ? input.index.table_uuid
                                                            : input.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &record.delta.table_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.row_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &record.delta.row_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(input.version_uuid,
                                    scratchbird::core::platform::UuidKind::row,
                                    &record.delta.version_uuid);
  if (diagnostic.error) { return diagnostic; }
  diagnostic = ParseLedgerTypedUuid(context.transaction_uuid.canonical,
                                    scratchbird::core::platform::UuidKind::transaction,
                                    &record.delta.transaction_uuid);
  if (diagnostic.error) { return diagnostic; }

  record.delta.local_transaction_id = context.local_transaction_id;
  record.delta.delta_kind = input.delta_kind;
  record.delta.key_payload = MakeSecondaryIndexDeltaKeyPayload(input.index, key, input.values);
  record.delta.cleanup_horizon_token =
      input.cleanup_horizon_token.empty()
          ? MakeSecondaryIndexDeltaCleanupHorizonToken(context)
          : input.cleanup_horizon_token;
  record.delta.committed = false;
  record.commit_state = idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.source_evidence_reference =
      input.source_evidence_reference.empty()
          ? MakeSecondaryIndexDeltaEvidenceReference(context, input, key)
          : input.source_evidence_reference;
  *out = std::move(record);
  return OkDiagnostic();
}

MgaSecondaryIndexDeltaLedgerResult LoadSecondaryIndexDeltaLedgerFromPath(
    const EngineRequestContext& context) {
  MgaSecondaryIndexDeltaLedgerResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                                    "database_path_required");
    return result;
  }
  const std::string path = SecondaryIndexDeltaLedgerStorePath(context);
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto bytes = ReadBinaryFile(path);
  if (bytes.empty()) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto decoded = idx::DecodePersistentSecondaryIndexDeltaLedger(
      bytes,
      DefaultSecondaryIndexDeltaLedgerLimits());
  if (!decoded.ok()) {
    result.diagnostic = DiagnosticFromSecondaryIndexDeltaLedger(
        decoded.diagnostic,
        "SB-MGA-SECONDARY-DELTA-LOAD-FAILED",
        "mga.secondary_index_delta_ledger.load_failed");
    return result;
  }
  result.ok = true;
  result.ledger = decoded.ledger;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineApiDiagnostic WriteSecondaryIndexDeltaLedger(
    const EngineRequestContext& context,
    const idx::PersistentSecondaryIndexDeltaLedger& ledger) {
  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(
      ledger,
      DefaultSecondaryIndexDeltaLedgerLimits());
  if (!encoded.ok()) {
    return DiagnosticFromSecondaryIndexDeltaLedger(
        encoded.diagnostic,
        "SB-MGA-SECONDARY-DELTA-ENCODE-FAILED",
        "mga.secondary_index_delta_ledger.encode_failed");
  }
  const std::string path = SecondaryIndexDeltaLedgerStorePath(context);
  const std::string tmp_path = path + ".tmp." +
                               std::to_string(context.local_transaction_id) + "." +
                               std::to_string(ledger.records.size());
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "ledger_store_append_failed");
    }
    out.write(reinterpret_cast<const char*>(encoded.bytes.data()),
              static_cast<std::streamsize>(encoded.bytes.size()));
    out.flush();
    if (!out) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "ledger_store_append_failed");
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(tmp_path, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(tmp_path, path, rename_error);
  }
  if (rename_error) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "ledger_store_replace_failed");
  }
  return OkDiagnostic();
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

std::string DecodeLineHexFieldOrRaw(const std::string& field) {
  if (field.rfind(kLineHexFieldPrefix, 0) != 0) {
    return field;
  }
  const std::string encoded =
      field.substr(kLineHexFieldPrefix.size());
  return DecodeCrudTextLocal(encoded);
}

bool LoadScopedBinaryIndexEntriesForTables(
    const EngineRequestContext& context,
    const std::set<std::string>& table_uuids,
    std::vector<CrudIndexEntryRecord>* entries,
    bool* used_segment) {
  if (entries == nullptr) {
    return false;
  }
  if (used_segment != nullptr) {
    *used_segment = false;
  }
  for (const auto& table_uuid : table_uuids) {
    const std::string path = ScopedIndexBinaryStorePath(context, table_uuid);
    if (!FileExistsAndNotEmpty(path)) {
      continue;
    }
    if (used_segment != nullptr) {
      *used_segment = true;
    }
    if (!DecodeScopedIndexBinaryStore(path, entries)) {
      return false;
    }
  }
  return true;
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

constexpr std::size_t kScopedDecodedRowCacheMaxAutoWarmRows = 60000;

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
      !control->cancellation_requested) {
    return false;
  }
  rows->clear();
  if (used_segment != nullptr) { *used_segment = false; }
  if (BoundedScopedReadCancelled(control)) { return false; }

  const std::string text_path = ScopedRowStorePath(context, table_uuid);
  const std::string binary_path =
      ScopedRowBinaryStorePath(context, table_uuid);
  const bool text_exists = FileExistsAndNotEmpty(text_path);
  const bool binary_exists = FileExistsAndNotEmpty(binary_path);
  if (!text_exists && !binary_exists) { return true; }
  if (used_segment != nullptr) { *used_segment = true; }

  const auto authorize_file = [&](const std::string& path,
                                  std::uint64_t* authorized_file_bytes) {
    if (authorized_file_bytes == nullptr) { return false; }
    std::error_code ignored;
    const auto size = std::filesystem::file_size(path, ignored);
    if (ignored || size == static_cast<std::uintmax_t>(-1) ||
        size > std::numeric_limits<std::uint64_t>::max()) {
      control->refusal_detail = "heap_read_scoped_segment_size_unavailable";
      return false;
    }
    *authorized_file_bytes = static_cast<std::uint64_t>(size);
    return AccountBoundedScopedBytes(control, *authorized_file_bytes);
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
  row_value_key_cache.reserve(64);
  if (text_exists) {
    std::ifstream input(text_path, std::ios::binary);
    if (!input) {
      control->refusal_detail = "heap_read_scoped_text_open_failed";
      return false;
    }
    const auto consume_line = [&](const std::string& line) {
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
      const std::uint64_t remaining =
          authorized_text_bytes - actual_text_bytes;
      const std::size_t requested = remaining == 0
                                        ? 1
                                        : static_cast<std::size_t>(std::min<
                                              std::uint64_t>(remaining,
                                                             kReadChunkBytes));
      input.read(chunk, static_cast<std::streamsize>(requested));
      const std::streamsize read_count = input.gcount();
      if (read_count < 0 ||
          static_cast<std::uint64_t>(read_count) > remaining) {
        control->refusal_detail = "heap_read_scoped_text_grew_during_read";
        return false;
      }
      actual_text_bytes += static_cast<std::uint64_t>(read_count);
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
        control->refusal_detail = "heap_read_scoped_text_read_failed";
        return false;
      }
      if (read_count < static_cast<std::streamsize>(requested)) {
        if (!input.eof()) {
          control->refusal_detail = "heap_read_scoped_text_read_failed";
          return false;
        }
        reached_eof = true;
      }
    }
    if (actual_text_bytes != authorized_text_bytes) {
      control->refusal_detail = "heap_read_scoped_text_changed_during_read";
      return false;
    }
    if (!line.empty() && !consume_line(line)) { return false; }
  }
  if (binary_exists) {
    ScopedRelationSummary binary_summary;
    if (!DecodeScopedRowBinaryStore(binary_path,
                                    &decoded_rows,
                                    &binary_summary,
                                    control,
                                    authorized_binary_bytes) ||
        binary_summary.malformed) {
      if (control->refusal_detail.empty()) {
        control->refusal_detail = "heap_read_scoped_binary_decode_failed";
      }
      return false;
    }
  }
  *rows = std::move(decoded_rows);
  return true;
}

std::string MakeMgaLargeValueLocator(const std::string& overflow_uuid,
                                     const std::string& content_hash,
                                     std::uint64_t total_bytes) {
  return "SBMGA_LARGE_VALUE:" + overflow_uuid + ":" + content_hash + ":" + std::to_string(total_bytes);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsMgaLargeValueLocator(const std::string& value) {
  return StartsWith(value, "SBMGA_LARGE_VALUE:");
}

EngineApiDiagnostic OverlayMgaTransactionAuthority(const EngineRequestContext& context,
                                                   CrudState* state);

struct LargeValueRecord {
  std::uint64_t total_bytes = 0;
  std::string content_hash;
  std::map<std::uint64_t, std::string> chunks;
};

struct LargeValueLoadResult {
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::map<std::string, std::string> locator_payloads;
  std::set<std::string> reclaimed_locators;
};

struct LargeValueReclaimLoadResult {
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::set<std::string> overflow_uuids;
};

LargeValueReclaimLoadResult LoadVisibleMgaLargeValueReclaims(
    const EngineRequestContext& context) {
  LargeValueReclaimLoadResult result;
  CrudState transaction_state;
  const auto authority = OverlayMgaTransactionAuthority(context, &transaction_state);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kRowStoreMagic ||
        fields[1] != "LARGE_VALUE_RECLAIMED") {
      continue;
    }
    const std::uint64_t creator_tx = ParseU64(fields[2]);
    if (CrudCreatorVisible(transaction_state,
                           creator_tx,
                           0,
                           context.local_transaction_id)) {
      result.overflow_uuids.insert(fields[3]);
    }
  }
  return result;
}

LargeValueLoadResult LoadMgaLargeValuePayloads(const EngineRequestContext& context) {
  LargeValueLoadResult result;
  const auto reclaimed = LoadVisibleMgaLargeValueReclaims(context);
  if (reclaimed.diagnostic.error) {
    result.diagnostic = reclaimed.diagnostic;
    return result;
  }
  std::map<std::string, LargeValueRecord> records;
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 2 || fields[0] != kRowStoreMagic) { continue; }
    if (fields[1] == "LARGE_VALUE" && fields.size() >= 11) {
      auto& record = records[fields[3]];
      record.total_bytes = ParseU64(fields[8]);
      record.content_hash = fields[9];
    } else if (fields[1] == "LARGE_VALUE_CHUNK" && fields.size() >= 7) {
      const std::string overflow_uuid = fields[3];
      const std::uint64_t ordinal = ParseU64(fields[4]);
      const std::string fragment = DecodeCrudTextLocal(fields[5]);
      const std::uint64_t expected_checksum = ParseU64(fields[6]);
      if (ChecksumText(fragment) != expected_checksum) {
        result.diagnostic = MakeInvalidRequestDiagnostic("mga.large_value", "large_value_chunk_checksum_mismatch");
        return result;
      }
      records[overflow_uuid].chunks[ordinal] = fragment;
    }
  }
  for (const auto& [overflow_uuid, record] : records) {
    const std::string locator =
        MakeMgaLargeValueLocator(overflow_uuid, record.content_hash, record.total_bytes);
    if (reclaimed.overflow_uuids.count(overflow_uuid) != 0) {
      result.reclaimed_locators.insert(locator);
      continue;
    }
    std::string payload;
    for (const auto& [ordinal, fragment] : record.chunks) {
      (void)ordinal;
      payload += fragment;
    }
    if (payload.size() != record.total_bytes ||
        std::to_string(ChecksumText(payload)) != record.content_hash) {
      result.diagnostic = MakeInvalidRequestDiagnostic("mga.large_value", "large_value_payload_checksum_mismatch");
      return result;
    }
    result.locator_payloads[locator] = payload;
  }
  return result;
}

EngineApiDiagnostic ExpandMgaLargeValueLocators(const EngineRequestContext& context,
                                                std::vector<CrudRowVersionRecord>* rows) {
  const auto payloads = LoadMgaLargeValuePayloads(context);
  if (payloads.diagnostic.error) { return payloads.diagnostic; }
  for (auto& row : *rows) {
    for (auto& [field, value] : row.values) {
      (void)field;
      if (!IsMgaLargeValueLocator(value)) { continue; }
      const auto payload_it = payloads.locator_payloads.find(value);
      if (payload_it == payloads.locator_payloads.end()) {
        if (payloads.reclaimed_locators.count(value) != 0) { continue; }
        return MakeInvalidRequestDiagnostic("mga.large_value", "large_value_locator_missing");
      }
      value = payload_it->second;
    }
  }
  return OkDiagnostic();
}

struct SavepointCutoffs {
  std::uint64_t row_event_sequence = 0;
  std::uint64_t metadata_event_sequence = 0;
  std::uint64_t index_event_sequence = 0;
};

struct SavepointRollbackRange {
  SavepointCutoffs cutoffs;
  std::uint64_t row_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t metadata_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t index_upper_event_sequence = std::numeric_limits<std::uint64_t>::max();
};

struct SavepointParsedState {
  std::map<std::uint64_t, std::map<std::string, SavepointCutoffs>> active_savepoints;
  std::map<std::uint64_t, std::vector<SavepointRollbackRange>> rollback_ranges;
};

SavepointParsedState ParseSavepoints(const EngineRequestContext& context) {
  SavepointParsedState state;
  for (const auto& line : ReadLines(SavepointStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 5 || fields[0] != kRowStoreMagic) { continue; }
    const std::string& kind = fields[1];
    const std::uint64_t tx = ParseU64(fields[2]);
    const std::string name = DecodeCrudTextLocal(fields[3]);
    SavepointCutoffs cutoffs;
    cutoffs.row_event_sequence = ParseU64(fields[4]);
    cutoffs.metadata_event_sequence = fields.size() >= 6 ? ParseU64(fields[5]) : cutoffs.row_event_sequence;
    cutoffs.index_event_sequence = fields.size() >= 7 ? ParseU64(fields[6]) : cutoffs.row_event_sequence;
    if (kind == "SAVEPOINT") {
      state.active_savepoints[tx][name] = cutoffs;
    } else if (kind == "RELEASE_SAVEPOINT") {
      const auto tx_it = state.active_savepoints.find(tx);
      if (tx_it != state.active_savepoints.end()) { tx_it->second.erase(name); }
    } else if (kind == "ROLLBACK_TO_SAVEPOINT") {
      SavepointRollbackRange range;
      range.cutoffs = cutoffs;
      if (fields.size() >= 10) {
        range.row_upper_event_sequence = ParseU64(fields[7]);
        range.metadata_upper_event_sequence = ParseU64(fields[8]);
        range.index_upper_event_sequence = ParseU64(fields[9]);
      }
      state.rollback_ranges[tx].push_back(range);
    }
  }
  return state;
}

bool RowEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                   std::uint64_t creator_tx,
                                   std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.row_event_sequence &&
        event_sequence <= range.row_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

bool MetadataEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                        std::uint64_t creator_tx,
                                        std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.metadata_event_sequence &&
        event_sequence <= range.metadata_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

bool IndexEventRolledBackBySavepoint(const SavepointParsedState& savepoints,
                                     std::uint64_t creator_tx,
                                     std::uint64_t event_sequence) {
  const auto ranges_it = savepoints.rollback_ranges.find(creator_tx);
  if (ranges_it == savepoints.rollback_ranges.end()) {
    return false;
  }
  for (const auto& range : ranges_it->second) {
    if (event_sequence > range.cutoffs.index_event_sequence &&
        event_sequence <= range.index_upper_event_sequence) {
      return true;
    }
  }
  return false;
}

struct DescriptorFieldsCacheRecord {
  std::uintmax_t file_size = 0;
  std::int64_t file_mtime_ticks = 0;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>>
      descriptors;
};

std::mutex& DescriptorFieldsCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, DescriptorFieldsCacheRecord>& DescriptorFieldsCache() {
  static std::map<std::string, DescriptorFieldsCacheRecord> cache;
  return cache;
}

struct MgaMetadataCacheKey {
  std::string database_uuid;
  std::string metadata_path;
  std::uintmax_t metadata_file_size = 0;
  std::int64_t metadata_file_mtime_ticks = 0;
  std::string savepoint_path;
  std::uintmax_t savepoint_file_size = 0;
  std::int64_t savepoint_file_mtime_ticks = 0;
  std::uint64_t local_transaction_id = 0;

  bool operator<(const MgaMetadataCacheKey& other) const {
    return std::tie(database_uuid,
                    metadata_path,
                    metadata_file_size,
                    metadata_file_mtime_ticks,
                    savepoint_path,
                    savepoint_file_size,
                    savepoint_file_mtime_ticks,
                    local_transaction_id) <
           std::tie(other.database_uuid,
                    other.metadata_path,
                    other.metadata_file_size,
                    other.metadata_file_mtime_ticks,
                    other.savepoint_path,
                    other.savepoint_file_size,
                    other.savepoint_file_mtime_ticks,
                    other.local_transaction_id);
  }
};

struct MgaMetadataCacheEntry {
  std::vector<CrudTableRecord> tables;
  std::vector<CrudIndexRecord> indexes;
  std::uint64_t max_event_sequence = 0;
};

std::mutex& MgaMetadataCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<MgaMetadataCacheKey, MgaMetadataCacheEntry>& MgaMetadataCache() {
  static std::map<MgaMetadataCacheKey, MgaMetadataCacheEntry> cache;
  return cache;
}

std::uintmax_t ExistingFileSize(const std::string& path) {
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return 0;
  }
  return std::filesystem::file_size(path, ignored);
}

ScopedRelationFileIdentity ExistingFileIdentity(const std::string& path) {
  std::error_code ignored;
  if (path.empty() || !std::filesystem::exists(path, ignored)) {
    return {};
  }
  return ScopedRelationTextFileIdentity(path);
}

std::map<std::string, std::vector<std::pair<std::string, std::string>>> LoadDescriptorFieldsByRelation(
    const EngineRequestContext& context,
    std::string_view required_relation_uuid = {}) {
  const std::string path = DescriptorStorePath(context);
  const auto identity = ExistingFileIdentity(path);
  const std::uintmax_t file_size = identity.ok ? identity.file_size : 0;
  const std::int64_t file_mtime_ticks =
      identity.ok ? identity.file_mtime_ticks : 0;
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    const auto cached = DescriptorFieldsCache().find(path);
    if (cached != DescriptorFieldsCache().end() &&
        cached->second.file_size == file_size &&
        cached->second.file_mtime_ticks == file_mtime_ticks &&
        (required_relation_uuid.empty() ||
         cached->second.descriptors.contains(
             std::string(required_relation_uuid)))) {
      return cached->second.descriptors;
    }
  }
  // Exact relation authority must not be refused solely by a negative cache
  // entry.  The descriptor store is append-published and can be populated by
  // another engine facade linked into the same server process; those facades
  // do not share this translation unit's in-memory cache.  When the caller
  // names an exact required relation and the matching cache entry omits it,
  // re-read the durable store even if its coarse file identity is unchanged.
  // A genuine durable miss remains fail-closed in the caller.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> descriptors;
  for (const auto& line : ReadLines(path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kDescriptorMagic || fields[1] != "RELATION") { continue; }
    descriptors[fields[2]] = DecodeCrudPairs(fields[3]);
  }
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    DescriptorFieldsCache()[path] = {file_size, file_mtime_ticks, descriptors};
  }
  return descriptors;
}

EngineApiDiagnostic PersistDescriptorFields(const EngineRequestContext& context,
                                            const std::string& relation_uuid,
                                            const std::vector<std::pair<std::string, std::string>>& fields) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "database_path_required");
  }
  const std::string line = JoinLine({kDescriptorMagic, "RELATION", relation_uuid, EncodeCrudPairs(fields)});
  const std::string path = DescriptorStorePath(context);
  if (!AppendLine(path, line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_descriptor", "descriptor_store_append_failed");
  }
  {
    const std::lock_guard<std::mutex> guard(DescriptorFieldsCacheMutex());
    auto cached = DescriptorFieldsCache().find(path);
    if (cached != DescriptorFieldsCache().end()) {
      const auto updated_identity = ExistingFileIdentity(path);
      cached->second.descriptors[relation_uuid] = fields;
      cached->second.file_size =
          updated_identity.ok ? updated_identity.file_size : 0;
      cached->second.file_mtime_ticks =
          updated_identity.ok ? updated_identity.file_mtime_ticks : 0;
    }
  }
  return OkDiagnostic();
}

std::string MgaTransactionStateName(TransactionState state) {
  switch (state) {
    case TransactionState::active: return "active";
    case TransactionState::read_only_active: return "read_only_active";
    case TransactionState::preparing: return "preparing";
    case TransactionState::prepared: return "prepared";
    case TransactionState::committing: return "committing";
    case TransactionState::committed: return "committed";
    case TransactionState::rolling_back: return "rolling_back";
    case TransactionState::rolled_back: return "rolled_back";
    case TransactionState::limbo: return "limbo";
    case TransactionState::recovering: return "recovering";
    case TransactionState::failed_terminal: return "failed_terminal";
    case TransactionState::archived: return "archived";
    case TransactionState::none:
    case TransactionState::created:
    default: return "none";
  }
}

EngineApiDiagnostic OverlayMgaTransactionAuthority(const EngineRequestContext& context, CrudState* state) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "state_required");
  }
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.transaction_authority", "database_path_required");
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) {
    return MakeEngineApiDiagnostic(loaded.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOAD-FAILED" : loaded.diagnostic.diagnostic_code,
                                   loaded.diagnostic.message_key.empty() ? "mga.transaction_inventory.load_failed" : loaded.diagnostic.message_key,
                                   loaded.diagnostic.remediation_hint,
                                   true);
  }
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    state->transactions[entry.identity.local_id.value] = MgaTransactionStateName(entry.state);
    state->max_transaction_id = std::max(state->max_transaction_id, entry.identity.local_id.value);
  }
  if (context.local_transaction_id != 0) {
    const auto lookup = LookupLocalTransaction(loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
    if (!lookup.ok()) {
      return MakeEngineApiDiagnostic(lookup.diagnostic.diagnostic_code.empty() ? "SB-MGA-TXN-INV-LOOKUP-FAILED" : lookup.diagnostic.diagnostic_code,
                                     lookup.diagnostic.message_key.empty() ? "mga.transaction_inventory.lookup_failed" : lookup.diagnostic.message_key,
                                     lookup.diagnostic.remediation_hint,
                                     true);
    }
    if (lookup.entry.state != TransactionState::active) {
      return MakeInvalidRequestDiagnostic("mga.transaction_authority", "active_local_transaction_required");
    }
  }
  return OkDiagnostic();
}

std::set<std::string> VisibleRetiredTemporaryTableMetadata(
    const EngineRequestContext& context,
    const CrudState& state) {
  std::set<std::string> retired_tables;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 7 || fields[0] != kRowStoreMagic ||
        fields[1] != "TABLE_METADATA_RETIRED") {
      continue;
    }
    const std::uint64_t creator_tx = ParseU64(fields[2]);
    const std::uint64_t event_sequence = ParseU64(fields[3]);
    const std::string& table_uuid = fields[4];
    const std::string& session_uuid = fields[6];
    if (!session_uuid.empty() &&
        session_uuid != context.session_uuid.canonical) {
      continue;
    }
    if (CrudCreatorVisible(state,
                           creator_tx,
                           event_sequence,
                           context.local_transaction_id)) {
      retired_tables.insert(table_uuid);
    }
  }
  return retired_tables;
}

void FilterVisibleRetiredTemporaryMetadata(const EngineRequestContext& context,
                                           CrudState* state) {
  if (state == nullptr) { return; }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, *state);
  if (retired_tables.empty()) { return; }
  state->tables.erase(std::remove_if(state->tables.begin(),
                                     state->tables.end(),
                                     [&retired_tables](const CrudTableRecord& table) {
                                       return retired_tables.count(table.table_uuid) != 0;
                                     }),
                      state->tables.end());
  state->indexes.erase(std::remove_if(state->indexes.begin(),
                                      state->indexes.end(),
                                      [&retired_tables](const CrudIndexRecord& index) {
                                        return retired_tables.count(index.table_uuid) != 0;
                                      }),
                       state->indexes.end());
  state->row_versions.erase(std::remove_if(state->row_versions.begin(),
                                           state->row_versions.end(),
                                           [&retired_tables](const CrudRowVersionRecord& row) {
                                             return retired_tables.count(row.table_uuid) != 0;
                                           }),
                            state->row_versions.end());
  state->index_entries.erase(std::remove_if(state->index_entries.begin(),
                                            state->index_entries.end(),
                                            [&retired_tables](const CrudIndexEntryRecord& entry) {
                                              return retired_tables.count(entry.table_uuid) != 0;
                                            }),
                             state->index_entries.end());
}

EngineApiDiagnostic ValidateMgaMutatingTransactionAuthority(const EngineRequestContext& context,
                                                           const std::string& operation_id) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  CrudState state;
  const auto authority = OverlayMgaTransactionAuthority(context, &state);
  if (authority.error) { return authority; }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaRowVersionRecordChains(
    const std::vector<CrudRowVersionRecord>& row_versions) {
  std::unordered_map<std::string, const CrudRowVersionRecord*> by_version_uuid;
  by_version_uuid.reserve(row_versions.size());
  for (const auto& row : row_versions) {
    if (row.version_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "row_version_uuid_required");
    }
    const auto inserted = by_version_uuid.emplace(row.version_uuid, &row);
    if (!inserted.second) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "duplicate_row_version_uuid");
    }
  }
  for (const auto& row : row_versions) {
    if (row.previous_version_uuid.empty()) { continue; }
    const auto previous = by_version_uuid.find(row.previous_version_uuid);
    if (previous == by_version_uuid.end()) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_missing");
    }
    const auto* previous_row = previous->second;
    if (previous_row->table_uuid != row.table_uuid ||
        previous_row->row_uuid != row.row_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_wrong_chain");
    }
    if (previous_row->sequence >= row.sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_not_older");
    }
    if (row.previous_sequence != 0 &&
        previous_row->sequence != row.previous_sequence) {
      return MakeInvalidRequestDiagnostic("mga.row_version_chain", "previous_row_version_sequence_mismatch");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaRowVersionChains(const CrudState& state) {
  return ValidateMgaRowVersionRecordChains(state.row_versions);
}

void FilterMgaTemporaryObjectsForSession(const EngineRequestContext& context, CrudState* state) {
  if (state == nullptr) { return; }
  std::map<std::string, bool> purged_tables;
  std::vector<CrudTableRecord> retained_tables;
  retained_tables.reserve(state->tables.size());
  for (const auto& table : state->tables) {
    const bool global_temporary_metadata =
        table.temporary && table.temporary_scope == "global";
    const bool visible = !table.temporary || global_temporary_metadata ||
                         (!context.session_uuid.canonical.empty() &&
                          table.temporary_session_uuid == context.session_uuid.canonical);
    if (visible) {
      retained_tables.push_back(table);
    } else {
      purged_tables[table.table_uuid] = true;
    }
  }
  state->tables = std::move(retained_tables);
  if (purged_tables.empty()) { return; }
  state->row_versions.erase(std::remove_if(state->row_versions.begin(), state->row_versions.end(),
                                           [&purged_tables](const CrudRowVersionRecord& row) {
                                             return purged_tables.count(row.table_uuid) != 0;
                                           }),
                            state->row_versions.end());
  state->indexes.erase(std::remove_if(state->indexes.begin(), state->indexes.end(),
                                      [&purged_tables](const CrudIndexRecord& index) {
                                        return purged_tables.count(index.table_uuid) != 0;
                                      }),
                       state->indexes.end());
  state->index_entries.erase(std::remove_if(state->index_entries.begin(), state->index_entries.end(),
                                            [&purged_tables](const CrudIndexEntryRecord& entry) {
                                              return purged_tables.count(entry.table_uuid) != 0;
                                            }),
                             state->index_entries.end());
}

EngineApiDiagnostic LoadMgaMetadata(CrudState* state, const EngineRequestContext& context) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "state_required");
  }
  const std::string metadata_path = MetadataStorePath(context);
  const std::string savepoint_path = SavepointStorePath(context);
  const auto metadata_identity = ExistingFileIdentity(metadata_path);
  const auto savepoint_identity = ExistingFileIdentity(savepoint_path);
  const MgaMetadataCacheKey cache_key{
      context.database_uuid.canonical,
      metadata_path,
      metadata_identity.ok ? metadata_identity.file_size : 0,
      metadata_identity.ok ? metadata_identity.file_mtime_ticks : 0,
      savepoint_path,
      savepoint_identity.ok ? savepoint_identity.file_size : 0,
      savepoint_identity.ok ? savepoint_identity.file_mtime_ticks : 0,
      context.local_transaction_id};
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    const auto cached = MgaMetadataCache().find(cache_key);
    if (cached != MgaMetadataCache().end()) {
      state->tables.insert(state->tables.end(),
                           cached->second.tables.begin(),
                           cached->second.tables.end());
      state->indexes.insert(state->indexes.end(),
                            cached->second.indexes.begin(),
                            cached->second.indexes.end());
      state->max_event_sequence =
          std::max(state->max_event_sequence,
                   cached->second.max_event_sequence);
      return OkDiagnostic();
    }
  }
  const auto savepoints = ParseSavepoints(context);
  MgaMetadataCacheEntry decoded;
  for (const auto& line : ReadLines(metadata_path)) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 4 || fields[0] != kRowStoreMagic) { continue; }
    if (fields[1] == "TABLE_METADATA") {
      if (fields.size() < 11) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_invalid");
      }
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[2]);
      table.event_sequence = ParseU64(fields[3]);
      table.table_uuid = fields[4];
      table.default_name = DecodeCrudTextLocal(fields[5]);
      table.columns = DecodeCrudPairs(fields[6]);
      table.temporary = fields[7] == "1";
      table.temporary_scope = fields[8];
      table.temporary_session_uuid = fields[9];
      table.on_commit_action = fields[10];
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             table.creator_tx,
                                             table.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, ParseU64(fields[3]));
      decoded.tables.push_back(std::move(table));
    } else if (fields[1] == "CONSTRAINT_MUTATION_BATCH") {
      // The constraint metadata and its table-column projection are sealed in
      // this one physical record.  The immutable relation-storage descriptor
      // UUID/generation remains the exact base binding and is not updated by
      // this bounded D1 bridge.
      namespace cbf = constraint_batch_field;
      if (fields.size() != cbf::kFieldCount ||
          fields[cbf::kMagic] != kRowStoreMagic ||
          fields[cbf::kRecordKind] != "CONSTRAINT_MUTATION_BATCH" ||
          ParseU64(fields[cbf::kCreatorTx]) == 0 ||
          ParseU64(fields[cbf::kEventSequence]) == 0 ||
          fields[cbf::kFormatVersion] != "neutral_fk_mutation_batch_v1" ||
          fields[cbf::kSealState] != "sealed" ||
          fields[cbf::kBatchHash].size() != 71 ||
          !fields[cbf::kBatchHash].starts_with("sha256:") ||
          fields[cbf::kMutationCount] != "1" ||
          !ValidConstraintBatchUuid(fields[cbf::kDatabaseUuid],
                                    scratchbird::core::platform::UuidKind::database) ||
          !ValidConstraintBatchUuid(fields[cbf::kBatchUuid],
                                    scratchbird::core::platform::UuidKind::row) ||
          !ValidConstraintBatchUuid(fields[cbf::kConstraintUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kOwnerTableUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kChildSchemaUuid],
                                    scratchbird::core::platform::UuidKind::schema) ||
          !ValidConstraintBatchUuid(fields[cbf::kChildDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          ParseU64(fields[cbf::kChildDescriptorGeneration]) == 0 ||
          !ValidConstraintBatchUuid(fields[cbf::kChildColumnUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentTableUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentSchemaUuid],
                                    scratchbird::core::platform::UuidKind::schema) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          ParseU64(fields[cbf::kParentDescriptorGeneration]) == 0 ||
          !ValidConstraintBatchUuid(fields[cbf::kParentColumnUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kParentCandidateConstraintUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kReferencedKeyDescriptorUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          !ValidConstraintBatchUuid(fields[cbf::kSupportUuid],
                                    scratchbird::core::platform::UuidKind::object) ||
          fields[cbf::kSupportFamily] != "btree" ||
          fields[cbf::kSupportPolicy] != "required_exact_unique_index" ||
          fields[cbf::kMatchPolicy] != "simple" ||
          fields[cbf::kOnUpdate] != "no_action" ||
          fields[cbf::kOnDelete] != "no_action" ||
          fields[cbf::kEnforcementTiming] != "immediate" ||
          ParseU64(fields[cbf::kConstraintMetadataGeneration]) != 1 ||
          ParseU64(fields[cbf::kBaseTableEventSequence]) == 0 ||
          ParseU64(fields[cbf::kParentBaseTableEventSequence]) == 0 ||
          fields[cbf::kConstraintKind] != "foreign_key" ||
          fields[cbf::kTableUuid] != fields[cbf::kOwnerTableUuid] ||
          fields[cbf::kDatabaseUuid] != context.database_uuid.canonical) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata", "constraint_mutation_batch_invalid");
      }
      MgaConstraintMutationBatch batch;
      batch.format_version = fields[cbf::kFormatVersion];
      batch.batch_uuid = fields[cbf::kBatchUuid];
      batch.batch_hash = fields[cbf::kBatchHash];
      batch.mutation_count = static_cast<std::uint32_t>(
          ParseU64(fields[cbf::kMutationCount]));
      batch.database_uuid = fields[cbf::kDatabaseUuid];
      batch.constraint_uuid = fields[cbf::kConstraintUuid];
      batch.owner_table_uuid = fields[cbf::kOwnerTableUuid];
      batch.child_schema_uuid = fields[cbf::kChildSchemaUuid];
      batch.child_relation_descriptor_uuid = fields[cbf::kChildDescriptorUuid];
      batch.child_relation_descriptor_generation =
          ParseU64(fields[cbf::kChildDescriptorGeneration]);
      batch.child_column_uuid = fields[cbf::kChildColumnUuid];
      batch.parent_table_uuid = fields[cbf::kParentTableUuid];
      batch.parent_schema_uuid = fields[cbf::kParentSchemaUuid];
      batch.parent_relation_descriptor_uuid = fields[cbf::kParentDescriptorUuid];
      batch.parent_relation_descriptor_generation =
          ParseU64(fields[cbf::kParentDescriptorGeneration]);
      batch.parent_column_uuid = fields[cbf::kParentColumnUuid];
      batch.parent_candidate_key_constraint_uuid =
          fields[cbf::kParentCandidateConstraintUuid];
      batch.key_descriptor_uuid = fields[cbf::kReferencedKeyDescriptorUuid];
      batch.support_uuid = fields[cbf::kSupportUuid];
      batch.support_family = fields[cbf::kSupportFamily];
      batch.support_policy = fields[cbf::kSupportPolicy];
      batch.match_policy = fields[cbf::kMatchPolicy];
      batch.on_update_action = fields[cbf::kOnUpdate];
      batch.on_delete_action = fields[cbf::kOnDelete];
      batch.enforcement_timing = fields[cbf::kEnforcementTiming];
      batch.constraint_metadata_generation =
          ParseU64(fields[cbf::kConstraintMetadataGeneration]);
      batch.base_table_event_sequence =
          ParseU64(fields[cbf::kBaseTableEventSequence]);
      batch.parent_base_table_event_sequence =
          ParseU64(fields[cbf::kParentBaseTableEventSequence]);
      batch.constraint_name = DecodeCrudTextLocal(fields[cbf::kConstraintName]);
      batch.constraint_kind = fields[cbf::kConstraintKind];
      batch.canonical_constraint_envelope =
          DecodeCrudTextLocal(fields[cbf::kCanonicalEnvelope]);
      CrudTableRecord table;
      table.creator_tx = ParseU64(fields[cbf::kCreatorTx]);
      table.event_sequence = ParseU64(fields[cbf::kEventSequence]);
      table.table_uuid = fields[cbf::kTableUuid];
      table.default_name = DecodeCrudTextLocal(fields[cbf::kTableDefaultName]);
      table.columns = DecodeCrudPairs(fields[cbf::kTableColumns]);
      table.temporary = fields[cbf::kTableTemporary] == "1";
      table.temporary_scope = fields[cbf::kTableTemporaryScope];
      table.temporary_session_uuid = fields[cbf::kTableTemporarySessionUuid];
      table.on_commit_action = fields[cbf::kTableOnCommitAction];
      if (table.temporary || !table.temporary_scope.empty() ||
          !table.temporary_session_uuid.empty() ||
          !table.on_commit_action.empty()) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "temporary_constraint_mutation_batch_unsupported");
      }
      batch.updated_table = table;
      const auto canonical_fields = ConstraintMutationBatchLineFields(
          batch, table.creator_tx, table.event_sequence);
      if (canonical_fields != fields) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "constraint_mutation_batch_noncanonical_encoding");
      }
      const std::string expected_hash =
          ComputeMgaConstraintMutationBatchHash(
              batch, table.creator_tx, table.event_sequence);
      if (expected_hash.empty() ||
          !scratchbird::core::hash::ConstantTimeEqual(expected_hash,
                                                       batch.batch_hash)) {
        return MakeInvalidRequestDiagnostic(
            "mga.relation_metadata",
            "constraint_mutation_batch_hash_mismatch");
      }
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             table.creator_tx,
                                             table.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, table.event_sequence);
      decoded.tables.push_back(std::move(table));
    } else if (fields[1] == "INDEX_METADATA") {
      if (fields.size() < 17) {
        return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_invalid");
      }
      CrudIndexRecord index;
      index.creator_tx = ParseU64(fields[2]);
      index.event_sequence = ParseU64(fields[3]);
      index.index_uuid = fields[4];
      index.table_uuid = fields[5];
      index.profile = NormalizeCrudIndexProfile(fields[6]);
      index.family = fields[7].empty() ? CrudIndexFamilyForProfile(index.profile) : fields[7];
      index.default_name = DecodeCrudTextLocal(fields[8]);
      index.column_name = DecodeCrudTextLocal(fields[9]);
      std::vector<std::string> key_envelopes;
      for (const auto& pair : DecodeCrudPairs(fields[10])) { key_envelopes.push_back(pair.second); }
      index.key_envelopes = std::move(key_envelopes);
      std::vector<std::string> include_columns;
      for (const auto& pair : DecodeCrudPairs(fields[11])) { include_columns.push_back(pair.second); }
      index.include_columns = std::move(include_columns);
      index.predicate_kind = fields[12];
      index.predicate_column = DecodeCrudTextLocal(fields[13]);
      index.predicate_value = DecodeCrudTextLocal(fields[14]);
      index.unique = fields[15] == "1";
      index.approximate = IsApproximateCrudIndexFamily(index.family);
      index.exact_fallback = index.approximate || fields[16] == "1";
      if (MetadataEventRolledBackBySavepoint(savepoints,
                                             index.creator_tx,
                                             index.event_sequence)) {
        continue;
      }
      decoded.max_event_sequence =
          std::max(decoded.max_event_sequence, ParseU64(fields[3]));
      decoded.indexes.push_back(std::move(index));
    }
  }
  state->tables.insert(state->tables.end(),
                       decoded.tables.begin(),
                       decoded.tables.end());
  state->indexes.insert(state->indexes.end(),
                        decoded.indexes.begin(),
                        decoded.indexes.end());
  state->max_event_sequence =
      std::max(state->max_event_sequence, decoded.max_event_sequence);
  {
    const std::lock_guard<std::mutex> guard(MgaMetadataCacheMutex());
    MgaMetadataCache()[cache_key] = std::move(decoded);
  }
  return OkDiagnostic();
}

std::string EncodeStringListAsCrudPairs(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t i = 0; i < values.size(); ++i) {
    pairs.push_back({std::to_string(i), values[i]});
  }
  return EncodeCrudPairs(pairs);
}

std::string RelationDescriptorTrimAscii(std::string value) {
  std::size_t first = 0;
  while (first < value.size() &&
         (value[first] == ' ' || value[first] == '\t' ||
          value[first] == '\n' || value[first] == '\r')) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         (value[last - 1] == ' ' || value[last - 1] == '\t' ||
          value[last - 1] == '\n' || value[last - 1] == '\r')) {
    --last;
  }
  return value.substr(first, last - first);
}

std::string RelationDescriptorLowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

std::map<std::string, std::string> RelationDescriptorFields(
    const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  std::string current;
  auto flush = [&fields](std::string part) {
    part = RelationDescriptorTrimAscii(std::move(part));
    if (part.empty()) { return; }
    const auto equals = part.find('=');
    if (equals == std::string::npos) {
      fields[RelationDescriptorLowerAscii(std::move(part))] = "true";
      return;
    }
    fields[RelationDescriptorLowerAscii(
        RelationDescriptorTrimAscii(part.substr(0, equals)))] =
            RelationDescriptorTrimAscii(part.substr(equals + 1));
  };
  for (char ch : descriptor) {
    if (ch == ';') {
      flush(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  flush(current);
  return fields;
}

std::optional<std::map<std::string, std::string>>
StrictRelationDescriptorFields(const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const std::size_t end = descriptor.find(';', start);
    std::string part = RelationDescriptorTrimAscii(descriptor.substr(
        start, end == std::string::npos ? std::string::npos : end - start));
    const auto equals = part.find('=');
    if (part.empty() || equals == std::string::npos || equals == 0) {
      return std::nullopt;
    }
    const std::string key = RelationDescriptorLowerAscii(
        RelationDescriptorTrimAscii(part.substr(0, equals)));
    if (key.empty() || fields.find(key) != fields.end()) {
      return std::nullopt;
    }
    fields.emplace(
        key, RelationDescriptorTrimAscii(part.substr(equals + 1)));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::string RelationDescriptorFieldOrEmpty(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) { return found->second; }
  }
  return {};
}

bool RelationDescriptorBoolField(
    const std::map<std::string, std::string>& fields,
    std::initializer_list<const char*> keys) {
  const std::string value = RelationDescriptorLowerAscii(
      RelationDescriptorFieldOrEmpty(fields, keys));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool RelationDescriptorRequiresDeferredStore(
    const std::map<std::string, std::string>& fields) {
  const std::string timing = RelationDescriptorLowerAscii(
      RelationDescriptorFieldOrEmpty(fields, {"enforcement_timing", "timing"}));
  if (timing == "deferred" || timing == "transaction_end" ||
      timing == "initially_deferred") {
    return true;
  }
  return RelationDescriptorBoolField(fields, {"deferrable", "initially_deferred"});
}

std::optional<std::string> ParentTableUuidFromRelationDescriptor(
    const std::string& descriptor) {
  const auto fields = RelationDescriptorFields(descriptor);
  auto field = [&fields](const char* key) -> std::string {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string{} : found->second;
  };
  std::string parent = field("referenced_table_uuid");
  if (parent.empty()) { parent = field("foreign_table_uuid"); }
  if (parent.empty()) { parent = field("foreign_table"); }
  if (!parent.empty()) { return parent; }
  std::string envelope = field("foreign_key");
  if (envelope.empty()) { envelope = field("references"); }
  if (envelope.empty()) { envelope = field("fk"); }
  if (envelope.empty()) { return std::nullopt; }
  envelope = RelationDescriptorTrimAscii(std::move(envelope));
  const auto colon = envelope.find(':');
  const auto dot = envelope.rfind('.');
  const auto open = envelope.find('(');
  if (colon != std::string::npos) {
    parent = envelope.substr(0, colon);
  } else if (dot != std::string::npos) {
    parent = envelope.substr(0, dot);
  } else if (open != std::string::npos) {
    parent = envelope.substr(0, open);
  }
  parent = RelationDescriptorTrimAscii(std::move(parent));
  if (parent.empty()) { return std::nullopt; }
  return parent;
}

std::set<std::string> InsertTargetRelationScope(const EngineRequestContext& context,
                                                const CrudState& metadata,
                                                const std::string& table_uuid) {
  std::set<std::string> table_scope;
  if (table_uuid.empty()) { return table_scope; }
  table_scope.insert(table_uuid);
  const auto table = FindVisibleCrudTable(metadata,
                                          table_uuid,
                                          context.local_transaction_id);
  if (!table) { return table_scope; }
  for (const auto& [column_name, descriptor] : table->columns) {
    (void)column_name;
    const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
    if (parent && !parent->empty()) {
      table_scope.insert(*parent);
    }
  }
  for (const auto& candidate : metadata.tables) {
    if (candidate.table_uuid.empty() || candidate.table_uuid == table_uuid) {
      continue;
    }
    if (!CrudCreatorVisible(metadata,
                            candidate.creator_tx,
                            candidate.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    for (const auto& [column_name, descriptor] : candidate.columns) {
      (void)column_name;
      const auto parent = ParentTableUuidFromRelationDescriptor(descriptor);
      if (parent && *parent == table_uuid) {
        table_scope.insert(candidate.table_uuid);
        break;
      }
    }
  }
  return table_scope;
}

bool RowsContainLargeValueLocators(const std::vector<CrudRowVersionRecord>& rows) {
  for (const auto& row : rows) {
    for (const auto& [field, value] : row.values) {
      (void)field;
      if (CrudValueIsLargeValueLocator(value)) {
        return true;
      }
      if (IsMgaLargeValueLocator(value)) {
        return true;
      }
    }
  }
  return false;
}

void AddRelationLoadEvidence(MgaRelationStoreResult* result,
                             const std::string& route) {
  if (result == nullptr) { return; }
  result->evidence.push_back({"mga_relation_state_load_route", route});
  result->evidence.push_back({"mga_relation_state_full_load",
                              result->full_state_load ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_scoped_load",
                              result->scoped_state_load ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_row_versions_scanned",
                              std::to_string(result->row_versions_scanned)});
  result->evidence.push_back({"mga_relation_state_row_versions_retained",
                              std::to_string(result->row_versions_retained)});
  result->evidence.push_back({"mga_relation_state_index_entries_scanned",
                              std::to_string(result->index_entries_scanned)});
  result->evidence.push_back({"mga_relation_state_index_entries_retained",
                              std::to_string(result->index_entries_retained)});
  result->evidence.push_back({"mga_relation_state_scoped_physical_segments",
                              result->scoped_physical_segments_used ? "true" : "false"});
  result->evidence.push_back({"mga_relation_state_scoped_physical_fallback",
                              result->scoped_physical_segments_fallback ? "true" : "false"});
}

}  // namespace

std::string ComputeMgaConstraintMutationBatchHash(
    const MgaConstraintMutationBatch& batch,
    std::uint64_t creator_local_transaction_id,
    std::uint64_t metadata_event_sequence) {
  if (creator_local_transaction_id == 0 || metadata_event_sequence == 0) {
    return {};
  }
  return ConstraintMutationBatchSha256(
      batch, creator_local_transaction_id, metadata_event_sequence);
}

MgaMetadataWorkPresenceResult HasVisibleMgaDeferredConstraintMetadata(
    const EngineRequestContext& context) {
  MgaMetadataWorkPresenceResult result;
  if (context.database_path.empty()) {
    result.diagnostic =
        MakeInvalidRequestDiagnostic("mga.deferred_constraint_metadata",
                                     "database_path_required");
    return result;
  }
  CrudState metadata;
  const auto loaded = LoadMgaMetadata(&metadata, context);
  if (loaded.error) {
    result.diagnostic = loaded;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  for (const auto& table : metadata.tables) {
    ++result.metadata_tables_scanned;
    if (!CrudCreatorVisible(metadata,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    for (const auto& [column_name, descriptor] : table.columns) {
      (void)column_name;
      if (RelationDescriptorRequiresDeferredStore(
              RelationDescriptorFields(descriptor))) {
        result.has_work = true;
        ++result.metadata_tables_matched;
        result.ok = true;
        result.diagnostic = OkDiagnostic();
        return result;
      }
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaMetadataWorkPresenceResult HasMgaTemporaryCleanupMetadataWork(
    const EngineRequestContext& context,
    bool include_delete_rows,
    bool include_preserve_rows,
    bool retire_private_metadata) {
  MgaMetadataWorkPresenceResult result;
  if (context.database_path.empty()) {
    result.diagnostic =
        MakeInvalidRequestDiagnostic("mga.temporary_cleanup_metadata",
                                     "database_path_required");
    return result;
  }
  CrudState metadata;
  const auto loaded = LoadMgaMetadata(&metadata, context);
  if (loaded.error) {
    result.diagnostic = loaded;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  for (const auto& table : metadata.tables) {
    ++result.metadata_tables_scanned;
    if (!table.temporary) { continue; }
    if (!CrudCreatorVisible(metadata,
                            table.creator_tx,
                            table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    const bool delete_rows_policy = table.on_commit_action == "delete_rows";
    const bool preserve_rows_policy = table.on_commit_action == "preserve_rows";
    if ((delete_rows_policy && include_delete_rows) ||
        (preserve_rows_policy && include_preserve_rows) ||
        retire_private_metadata) {
      result.has_work = true;
      ++result.metadata_tables_matched;
      result.ok = true;
      result.diagnostic = OkDiagnostic();
      return result;
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreState(const EngineRequestContext& context) {
  MgaRelationStoreResult result;
  result.full_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  const auto metadata = LoadMgaMetadata(&result.state.crud_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &result.state.crud_metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto savepoints = ParseSavepoints(context);
  std::unordered_map<std::string, std::string> row_value_key_cache;
  std::set<std::string> all_table_uuids;
  for (const auto& table : result.state.crud_metadata.tables) {
    if (!table.table_uuid.empty()) {
      all_table_uuids.insert(table.table_uuid);
    }
  }
  const auto discovered_scoped_tables = DiscoverScopedRelationTableUuids(context);
  all_table_uuids.insert(discovered_scoped_tables.begin(),
                         discovered_scoped_tables.end());
  std::set<std::string> scoped_row_tables_used;
  for (const auto& table_uuid : all_table_uuids) {
    std::vector<CrudRowVersionRecord> decoded_rows;
    bool used_segment = false;
    if (!LoadDecodedScopedRowsForTable(context,
                                       table_uuid,
                                       &decoded_rows,
                                       &used_segment)) {
      result.diagnostic = MakeInvalidRequestDiagnostic(
          "mga.row_store",
          "scoped_row_segment_decode_failed");
      return result;
    }
    if (!used_segment) { continue; }
    scoped_row_tables_used.insert(table_uuid);
    result.row_versions_scanned +=
        static_cast<std::uint64_t>(decoded_rows.size());
    for (auto& row : decoded_rows) {
      if (RowEventRolledBackBySavepoint(savepoints,
                                        row.creator_tx,
                                        row.event_sequence)) {
        continue;
      }
      result.state.max_row_event_sequence =
          std::max(result.state.max_row_event_sequence, row.event_sequence);
      result.state.row_versions.push_back(std::move(row));
      ++result.row_versions_retained;
    }
  }
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      continue;
    }
    if (scoped_row_tables_used.count(fields[4]) != 0) {
      continue;
    }
    ++result.row_versions_scanned;
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
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      continue;
    }
    result.state.max_row_event_sequence =
        std::max(result.state.max_row_event_sequence, row.event_sequence);
    result.state.row_versions.push_back(std::move(row));
    ++result.row_versions_retained;
  }
  bool scoped_index_segments_used = false;
  bool scoped_binary_index_segments_used = false;
  std::vector<CrudIndexEntryRecord> binary_index_entries;
  if (!LoadScopedBinaryIndexEntriesForTables(context,
                                             all_table_uuids,
                                             &binary_index_entries,
                                             &scoped_binary_index_segments_used)) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.index_store",
        "scoped_index_binary_segment_decode_failed");
    return result;
  }
  std::vector<std::string> index_lines =
      ReadScopedRelationLinesForTables(context,
                                       all_table_uuids,
                                       false,
                                       &scoped_index_segments_used);
  if (!scoped_index_segments_used && !scoped_binary_index_segments_used) {
    index_lines = ReadLines(IndexStorePath(context));
  }
  result.scoped_physical_segments_used =
      !scoped_row_tables_used.empty() || scoped_index_segments_used ||
      scoped_binary_index_segments_used;
  result.scoped_physical_segments_fallback =
      scoped_row_tables_used.empty() && !scoped_index_segments_used &&
      !scoped_binary_index_segments_used;

  for (const auto& line : index_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 13 || fields[0] != kRowStoreMagic || fields[1] != "INDEX_ENTRY") { continue; }
    ++result.index_entries_scanned;
    CrudIndexEntryRecord entry;
    entry.creator_tx = ParseU64(fields[2]);
    entry.event_sequence = ParseU64(fields[3]);
    entry.sequence = entry.event_sequence;
    entry.index_uuid = fields[4];
    entry.table_uuid = fields[5];
    entry.column_name = fields[6];
    entry.family = fields[7];
    entry.entry_kind = fields[8];
    entry.key_value = DecodeLineHexFieldOrRaw(fields[9]);
    entry.payload_value = DecodeLineHexFieldOrRaw(fields[10]);
    entry.row_uuid = fields[11];
    entry.version_uuid = fields[12];
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence = std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }
  for (auto& entry : binary_index_entries) {
    ++result.index_entries_scanned;
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence =
        std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.crud_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.crud_metadata);
  result.state.row_versions.erase(
      std::remove_if(result.state.row_versions.begin(),
                     result.state.row_versions.end(),
                     [&retired_tables](const CrudRowVersionRecord& row) {
                       return retired_tables.count(row.table_uuid) != 0;
                     }),
      result.state.row_versions.end());
  result.state.index_entries.erase(
      std::remove_if(result.state.index_entries.begin(),
                     result.state.index_entries.end(),
                     [&retired_tables](const CrudIndexEntryRecord& entry) {
                       return retired_tables.count(entry.table_uuid) != 0;
                     }),
      result.state.index_entries.end());
  if (RowsContainLargeValueLocators(result.state.row_versions)) {
    const auto expanded_large_values = ExpandMgaLargeValueLocators(context, &result.state.row_versions);
    if (expanded_large_values.error) {
      result.diagnostic = expanded_large_values;
      return result;
    }
  }
  const auto chain_status =
      ValidateMgaRowVersionRecordChains(result.state.row_versions);
  if (chain_status.error) {
    result.diagnostic = chain_status;
    return result;
  }
  FilterMgaTemporaryObjectsForSession(context, &result.state.crud_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, "full");
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreStateForTargetScope(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids,
    const std::string& evidence_route,
    bool include_index_entries = true,
    bool include_row_versions = true) {
  MgaRelationStoreResult result;
  result.scoped_state_load = true;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
    return result;
  }
  if (table_uuids.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store", "target_table_uuid_required");
    return result;
  }
  for (const auto& table_uuid : table_uuids) {
    if (table_uuid.empty()) {
      result.diagnostic =
          MakeInvalidRequestDiagnostic("mga.row_store",
                                       "target_table_uuid_required");
      return result;
    }
  }
  const auto metadata = LoadMgaMetadata(&result.state.crud_metadata, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &result.state.crud_metadata);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  const auto retired_tables =
      VisibleRetiredTemporaryTableMetadata(context, result.state.crud_metadata);
  FilterVisibleRetiredTemporaryMetadata(context, &result.state.crud_metadata);
  std::set<std::string> table_scope;
  for (const auto& table_uuid : table_uuids) {
    const auto scoped =
        InsertTargetRelationScope(context, result.state.crud_metadata, table_uuid);
    table_scope.insert(scoped.begin(), scoped.end());
  }
  const auto savepoints = ParseSavepoints(context);

  bool row_segments_used = false;
  bool index_segments_used = false;
  bool binary_index_segments_used = false;
  std::vector<CrudIndexEntryRecord> binary_index_entries;
  std::vector<std::string> index_lines;
  if (include_index_entries) {
    if (!LoadScopedBinaryIndexEntriesForTables(context,
                                               table_scope,
                                               &binary_index_entries,
                                               &binary_index_segments_used)) {
      result.diagnostic = MakeInvalidRequestDiagnostic(
          "mga.index_store",
          "scoped_index_binary_segment_decode_failed");
      return result;
    }
    index_lines = ReadScopedRelationLinesForTables(context,
                                                   table_scope,
                                                   false,
                                                   &index_segments_used);
  }

  if (include_row_versions) {
    for (const auto& scoped_table_uuid : table_scope) {
      std::vector<CrudRowVersionRecord> decoded_rows;
      bool used_segment = false;
      if (!LoadDecodedScopedRowsForTable(context,
                                         scoped_table_uuid,
                                         &decoded_rows,
                                         &used_segment)) {
        result.diagnostic = MakeInvalidRequestDiagnostic(
            "mga.row_store",
            "scoped_row_segment_decode_failed");
        return result;
      }
      row_segments_used = row_segments_used || used_segment;
      result.row_versions_scanned +=
          static_cast<std::uint64_t>(decoded_rows.size());
      for (auto& row : decoded_rows) {
        if (table_scope.count(row.table_uuid) == 0 ||
            retired_tables.count(row.table_uuid) != 0) {
          continue;
        }
        if (RowEventRolledBackBySavepoint(savepoints,
                                          row.creator_tx,
                                          row.event_sequence)) {
          continue;
        }
        result.state.max_row_event_sequence =
            std::max(result.state.max_row_event_sequence, row.event_sequence);
        result.state.row_versions.push_back(std::move(row));
        ++result.row_versions_retained;
      }
    }
  }
  result.scoped_physical_segments_used =
      row_segments_used || index_segments_used || binary_index_segments_used;
  result.scoped_physical_segments_fallback = false;

  for (const auto& line : index_lines) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 13 || fields[0] != kRowStoreMagic || fields[1] != "INDEX_ENTRY") { continue; }
    ++result.index_entries_scanned;
    if (table_scope.count(fields[5]) == 0 || retired_tables.count(fields[5]) != 0) {
      continue;
    }
    CrudIndexEntryRecord entry;
    entry.creator_tx = ParseU64(fields[2]);
    entry.event_sequence = ParseU64(fields[3]);
    entry.sequence = entry.event_sequence;
    entry.index_uuid = fields[4];
    entry.table_uuid = fields[5];
    entry.column_name = fields[6];
    entry.family = fields[7];
    entry.entry_kind = fields[8];
    entry.key_value = DecodeLineHexFieldOrRaw(fields[9]);
    entry.payload_value = DecodeLineHexFieldOrRaw(fields[10]);
    entry.row_uuid = fields[11];
    entry.version_uuid = fields[12];
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence =
        std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }
  for (auto& entry : binary_index_entries) {
    ++result.index_entries_scanned;
    if (table_scope.count(entry.table_uuid) == 0 ||
        retired_tables.count(entry.table_uuid) != 0) {
      continue;
    }
    if (IndexEventRolledBackBySavepoint(savepoints,
                                        entry.creator_tx,
                                        entry.event_sequence)) {
      continue;
    }
    result.state.max_index_event_sequence =
        std::max(result.state.max_index_event_sequence, entry.event_sequence);
    result.state.index_entries.push_back(std::move(entry));
    ++result.index_entries_retained;
  }

  if (include_row_versions && RowsContainLargeValueLocators(result.state.row_versions)) {
    const auto expanded_large_values =
        ExpandMgaLargeValueLocators(context, &result.state.row_versions);
    if (expanded_large_values.error) {
      result.diagnostic = expanded_large_values;
      return result;
    }
  }
  if (include_row_versions) {
    const auto chain_status =
        ValidateMgaRowVersionRecordChains(result.state.row_versions);
    if (chain_status.error) {
      result.diagnostic = chain_status;
      return result;
    }
  }
  FilterMgaTemporaryObjectsForSession(context, &result.state.crud_metadata);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddRelationLoadEvidence(&result, evidence_route);
  result.evidence.push_back({"mga_relation_state_row_versions_loaded",
                             include_row_versions ? "true" : "false"});
  result.evidence.push_back({"mga_relation_state_index_entries_loaded",
                             include_index_entries ? "true" : "false"});
  return result;
}

MgaRelationStoreResult LoadMgaRelationStoreStateForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreIndexesOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_index_only_scoped",
      true,
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreMetadataOnlyForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "insert_target_metadata_only_scoped",
      false,
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "mutation_target_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreStateForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids) {
  return LoadMgaRelationStoreStateForTargetScope(context,
                                                table_uuids,
                                                "mutation_targets_scoped");
}

MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  return LoadMgaRelationStoreStateForTargetScope(
      context,
      std::vector<std::string>{table_uuid},
      "mutation_target_rows_only_scoped",
      false);
}

MgaRelationStoreResult LoadMgaRelationStoreRowsOnlyForMutationTargets(
    const EngineRequestContext& context,
    const std::vector<std::string>& table_uuids) {
  return LoadMgaRelationStoreStateForTargetScope(context,
                                                table_uuids,
                                                "mutation_targets_rows_only_scoped",
                                                false);
}

std::uint64_t CurrentMgaRelationMetadataEventSequence(
    const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return 0;
  }
  const std::uint64_t next = ScanNextMetadataEventSequence(context);
  return next == 0 ? 0 : next - 1;
}

MgaRelationIndexOnlyProofEligibilityResult
CanUseMgaRelationIndexOnlyProofForInsertTarget(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaRelationIndexOnlyProofEligibilityResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store",
                                                     "database_path_required");
    result.refusal_reason = "database_path_required";
    return result;
  }
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.row_store",
                                                     "target_table_uuid_required");
    result.refusal_reason = "target_table_uuid_required";
    return result;
  }

  const std::lock_guard<std::mutex> guard(ScopedRelationSummaryMutex());
  const ScopedRelationSummary summary = LoadScopedRelationSummary(context,
                                                                  table_uuid);
  result.summary_trusted = summary.trusted && !summary.malformed;
  result.row_version_count = summary.row_version_count;
  result.tombstone_count = summary.tombstone_count;
  result.update_count = summary.update_count;
  result.evidence.push_back({"mga_relation_index_only_summary_trusted",
                             result.summary_trusted ? "true" : "false"});
  result.evidence.push_back({"mga_relation_index_only_row_versions",
                             std::to_string(result.row_version_count)});
  result.evidence.push_back({"mga_relation_index_only_tombstones",
                             std::to_string(result.tombstone_count)});
  result.evidence.push_back({"mga_relation_index_only_updates",
                             std::to_string(result.update_count)});
  if (summary.malformed) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.row_store",
        "scoped_relation_summary_malformed");
    result.refusal_reason = "scoped_relation_summary_malformed";
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (!summary.trusted) {
    result.refusal_reason = "scoped_relation_summary_untrusted";
  } else if (summary.tombstone_count != 0) {
    result.refusal_reason = "scoped_relation_tombstones_present";
  } else if (summary.update_count != 0) {
    result.refusal_reason = "scoped_relation_updates_present";
  } else {
    result.eligible = true;
    result.refusal_reason = "eligible";
  }
  result.evidence.push_back({"mga_relation_index_only_eligible",
                             result.eligible ? "true" : "false"});
  result.evidence.push_back({"mga_relation_index_only_reason",
                             result.refusal_reason});
  return result;
}

CrudState BuildCrudCompatibilityStateFromMga(const MgaRelationStoreState& state) {
  CrudState merged = state.crud_metadata;
  merged.row_versions = state.row_versions;
  merged.index_entries = state.index_entries;
  merged.max_sequence = state.max_row_event_sequence;
  merged.max_index_sequence = state.max_index_event_sequence;
  return merged;
}

CrudState BuildCrudCompatibilityStateFromMga(MgaRelationStoreState&& state) {
  CrudState merged = std::move(state.crud_metadata);
  merged.row_versions = std::move(state.row_versions);
  merged.index_entries = std::move(state.index_entries);
  merged.max_sequence = state.max_row_event_sequence;
  merged.max_index_sequence = state.max_index_event_sequence;
  return merged;
}

MgaTemporaryTableVisibilityResult CheckMgaTemporaryTableVisibility(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaTemporaryTableVisibilityResult result;
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("mga.temporary_table_visibility",
                                                     "table_uuid_required");
    return result;
  }
  CrudState state;
  const auto metadata = LoadMgaMetadata(&state, context);
  if (metadata.error) {
    result.diagnostic = metadata;
    return result;
  }
  bool has_table_candidate = false;
  bool temporary_table_candidate = false;
  for (const auto& table : state.tables) {
    if (table.table_uuid == table_uuid) {
      has_table_candidate = true;
      temporary_table_candidate = table.temporary;
      break;
    }
  }
  if (!has_table_candidate) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  const auto authority = OverlayMgaTransactionAuthority(context, &state);
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }
  FilterVisibleRetiredTemporaryMetadata(context, &state);
  const auto visible = FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (!visible) {
    result.known_temporary = temporary_table_candidate;
    result.hidden_by_temporary_visibility = temporary_table_candidate;
    return result;
  }
  result.table_visible = true;
  result.table = *visible;
  if (!visible->temporary) { return result; }
  result.known_temporary = true;
  result.visible_to_session =
      visible->temporary_scope == "global" ||
      (!visible->temporary_session_uuid.empty() &&
       visible->temporary_session_uuid == context.session_uuid.canonical);
  return result;
}

MgaTemporaryRecoveryClassificationResult ClassifyMgaTemporaryRecoveryState(
    const EngineRequestContext& context) {
  struct LatestRowState {
    std::uint64_t event_sequence = 0;
    bool deleted = false;
  };
  enum class EventAuthority {
    kCommitted,
    kRolledBack,
    kActiveOrUnresolved,
    kFenced,
  };

  MgaTemporaryRecoveryClassificationResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.temporary_recovery",
        "database_path_required");
    return result;
  }
  const auto loaded = LoadLocalTransactionInventoryFromDatabase(
      context.database_path);
  if (!loaded.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        loaded.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : loaded.diagnostic.diagnostic_code,
        loaded.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : loaded.diagnostic.message_key,
        loaded.diagnostic.remediation_hint,
        true);
    return result;
  }
  std::map<std::uint64_t, std::string> transaction_states;
  for (const auto& entry : loaded.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    transaction_states[entry.identity.local_id.value] =
        MgaTransactionStateName(entry.state);
  }
  auto classify_event = [&](std::uint64_t creator_tx) {
    if (creator_tx == 0) { return EventAuthority::kCommitted; }
    const auto found = transaction_states.find(creator_tx);
    if (found == transaction_states.end()) {
      ++result.fenced_event_count;
      return EventAuthority::kFenced;
    }
    if (found->second == "committed" || found->second == "archived") {
      return EventAuthority::kCommitted;
    }
    if (found->second == "rolled_back") {
      ++result.rolled_back_event_count;
      return EventAuthority::kRolledBack;
    }
    ++result.active_or_unresolved_event_count;
    return EventAuthority::kActiveOrUnresolved;
  };

  std::set<std::string> temporary_tables;
  std::set<std::string> durable_global_tables;
  std::set<std::string> committed_private_tables;
  std::set<std::string> retired_private_tables;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "TABLE_METADATA" && fields[7] == "1") {
      temporary_tables.insert(fields[4]);
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority != EventAuthority::kCommitted) { continue; }
      if (fields[8] == "global") {
        durable_global_tables.insert(fields[4]);
      } else {
        committed_private_tables.insert(fields[4]);
      }
    } else if (fields.size() >= 7 && fields[0] == kRowStoreMagic &&
               fields[1] == "TABLE_METADATA_RETIRED") {
      temporary_tables.insert(fields[4]);
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        retired_private_tables.insert(fields[4]);
        ++result.retired_private_metadata_count;
      }
    }
  }
  result.durable_global_metadata_count =
      static_cast<std::uint64_t>(durable_global_tables.size());
  for (const auto& table_uuid : committed_private_tables) {
    if (retired_private_tables.count(table_uuid) == 0) {
      ++result.orphaned_private_metadata_count;
    }
  }

  std::map<std::string, LatestRowState> latest_rows;
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 12 || fields[0] != kRowStoreMagic ||
        fields[1] != "ROW_VERSION") {
      continue;
    }
    const std::string& table_uuid = fields[4];
    const std::string& row_uuid = fields[5];
    const std::string& session_uuid = fields[11];
    if (session_uuid.empty() && temporary_tables.count(table_uuid) == 0) {
      continue;
    }
    temporary_tables.insert(table_uuid);
    const auto authority = classify_event(ParseU64(fields[2]));
    if (authority != EventAuthority::kCommitted) { continue; }
    const std::uint64_t event_sequence = ParseU64(fields[3]);
    const std::string key = table_uuid + "\t" + row_uuid + "\t" + session_uuid;
    auto& latest = latest_rows[key];
    if (event_sequence >= latest.event_sequence) {
      latest.event_sequence = event_sequence;
      latest.deleted = fields[7] == "1";
    }
  }
  for (const auto& [_, row] : latest_rows) {
    if (row.deleted) {
      ++result.cleaned_row_count;
    } else {
      ++result.orphaned_row_count;
    }
  }

  std::set<std::string> committed_large_values;
  std::set<std::string> reclaimed_large_values;
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "LARGE_VALUE") {
      if (temporary_tables.count(fields[4]) == 0) { continue; }
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        committed_large_values.insert(fields[3]);
      }
    } else if (fields.size() >= 9 && fields[0] == kRowStoreMagic &&
               fields[1] == "LARGE_VALUE_RECLAIMED") {
      if (temporary_tables.count(fields[4]) == 0) { continue; }
      const auto authority = classify_event(ParseU64(fields[2]));
      if (authority == EventAuthority::kCommitted) {
        reclaimed_large_values.insert(fields[3]);
      }
    }
  }
  result.reclaimed_large_value_count =
      static_cast<std::uint64_t>(reclaimed_large_values.size());
  for (const auto& overflow_uuid : committed_large_values) {
    if (reclaimed_large_values.count(overflow_uuid) == 0) {
      ++result.orphaned_large_value_count;
    }
  }

  if (result.active_or_unresolved_event_count != 0) {
    result.classification = "recovery_required";
    result.action = "transaction_recovery_required_before_open";
    result.recovery_required = true;
    result.write_admission_must_remain_fenced = true;
  } else if (result.fenced_event_count != 0) {
    result.classification = "fenced";
    result.action = "operator_recovery_required_missing_transaction_authority";
    result.write_admission_must_remain_fenced = true;
  } else if (result.orphaned_private_metadata_count != 0 ||
             result.orphaned_row_count != 0 ||
             result.orphaned_large_value_count != 0) {
    result.classification = "recovery_required";
    result.action = "temporary_orphan_cleanup_required_before_open";
    result.recovery_required = true;
    result.write_admission_must_remain_fenced = true;
  } else if (result.durable_global_metadata_count != 0 ||
             result.cleaned_row_count != 0 ||
             result.reclaimed_large_value_count != 0 ||
             result.retired_private_metadata_count != 0) {
    result.classification = "new_state";
    result.action = "open_allowed_no_orphaned_temporary_state";
  } else {
    result.classification = "old_state";
    result.action = "open_allowed_no_visible_temporary_state";
  }
  result.evidence.push_back({"temporary_recovery_classification",
                             result.classification});
  result.evidence.push_back({"temporary_recovery_action", result.action});
  result.evidence.push_back({"temporary_recovery_active_or_unresolved_events",
                             std::to_string(result.active_or_unresolved_event_count)});
  result.evidence.push_back({"temporary_recovery_fenced_events",
                             std::to_string(result.fenced_event_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_private_metadata",
                             std::to_string(result.orphaned_private_metadata_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_rows",
                             std::to_string(result.orphaned_row_count)});
  result.evidence.push_back({"temporary_recovery_orphaned_large_values",
                             std::to_string(result.orphaned_large_value_count)});
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

MgaRelationPhysicalSweepResult ApplyMgaRelationPhysicalSweepToState(
    const MgaRelationPhysicalSweepRequest& request) {
  namespace mga = scratchbird::transaction::mga;
  auto fail = [](std::string detail) {
    MgaRelationPhysicalSweepResult result;
    result.fail_closed = true;
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_physical_sweep", std::move(detail));
    return result;
  };

  if (!request.engine_mga_authoritative) {
    return fail("engine_mga_authority_required");
  }
  if (!request.cleanup_horizon_authoritative ||
      request.authoritative_cleanup_horizon_local_transaction_id == 0) {
    return fail("cleanup_horizon_authority_required");
  }
  if (request.max_row_versions_to_scan == 0 ||
      request.state.row_versions.size() > request.max_row_versions_to_scan) {
    return fail("bounded_row_version_scan_required");
  }
  if (request.max_index_entries_to_scan == 0 ||
      request.state.index_entries.size() > request.max_index_entries_to_scan) {
    return fail("bounded_index_entry_scan_required");
  }
  if (request.reclaim_evidence_records.empty()) {
    return fail("reclaim_evidence_required");
  }

  auto evidence_matches_row =
      [](const mga::LocalCleanupReclaimEvidenceRecord& evidence,
         const CrudRowVersionRecord& row) {
        const std::string row_uuid =
            scratchbird::core::uuid::UuidToString(
                evidence.row_version_identity.row.row_uuid.value);
        return row.creator_tx ==
                   evidence.row_version_identity.creator_transaction.local_id.value &&
               row.sequence == evidence.row_version_identity.version_sequence &&
               row.row_uuid == row_uuid;
      };
  auto evidence_for_row =
      [&](const CrudRowVersionRecord& row)
          -> const mga::LocalCleanupReclaimEvidenceRecord* {
        for (const auto& evidence : request.reclaim_evidence_records) {
          if (evidence_matches_row(evidence, row)) {
            return &evidence;
          }
        }
        return nullptr;
      };

  MgaRelationPhysicalSweepResult result;
  result.state = request.state;
  result.state.row_versions.clear();
  result.state.index_entries.clear();
  result.scanned_row_version_count =
      static_cast<std::uint64_t>(request.state.row_versions.size());
  result.scanned_index_entry_count =
      static_cast<std::uint64_t>(request.state.index_entries.size());

  std::vector<std::string> removed_version_uuids;
  std::vector<std::string> matched_evidence_ids;
  for (const auto& row : request.state.row_versions) {
    const auto* evidence = evidence_for_row(row);
    if (evidence == nullptr) {
      result.state.row_versions.push_back(row);
      ++result.retained_row_version_count;
      continue;
    }
    removed_version_uuids.push_back(row.version_uuid);
    matched_evidence_ids.push_back(evidence->stable_evidence_id);
    ++result.removed_row_version_count;
  }

  for (const auto& evidence : request.reclaim_evidence_records) {
    if (std::find(matched_evidence_ids.begin(),
                  matched_evidence_ids.end(),
                  evidence.stable_evidence_id) == matched_evidence_ids.end()) {
      return fail("reclaim_evidence_not_in_relation_state:" +
                  evidence.stable_evidence_id);
    }
  }

  for (const auto& entry : request.state.index_entries) {
    if (std::find(removed_version_uuids.begin(),
                  removed_version_uuids.end(),
                  entry.version_uuid) != removed_version_uuids.end()) {
      ++result.removed_index_entry_count;
      continue;
    }
    result.state.index_entries.push_back(entry);
    ++result.retained_index_entry_count;
  }

  result.ok = true;
  result.physical_state_mutated =
      result.removed_row_version_count != 0 ||
      result.removed_index_entry_count != 0;
  result.diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.evidence.push_back({"mga_relation_physical_sweep",
                             "durable_mga_cleanup_horizon:" +
                                 std::to_string(
                                     request
                                         .authoritative_cleanup_horizon_local_transaction_id)});
  result.evidence.push_back({"mga_relation_physical_sweep_authority",
                             "durable_mga_transaction_inventory"});
  return result;
}

namespace {

void AddBytes(std::uint64_t* total, std::uint64_t bytes) {
  if (total == nullptr) { return; }
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  *total = bytes > max - *total ? max : *total + bytes;
}

std::uint64_t TextBytes(const std::string& value) {
  return static_cast<std::uint64_t>(value.size());
}

std::uint64_t PairBytes(const std::vector<std::pair<std::string, std::string>>& values) {
  std::uint64_t total = 0;
  for (const auto& [name, value] : values) {
    AddBytes(&total, 8 + TextBytes(name) + TextBytes(value));
  }
  return total;
}

std::uint64_t StringListBytes(const std::vector<std::string>& values) {
  std::uint64_t total = 0;
  for (const auto& value : values) {
    AddBytes(&total, 8 + TextBytes(value));
  }
  return total;
}

std::uint64_t TableMetadataEstimateBytes(const CrudTableRecord& table) {
  std::uint64_t total = 96;
  AddBytes(&total, TextBytes(table.table_uuid));
  AddBytes(&total, TextBytes(table.default_name));
  AddBytes(&total, PairBytes(table.columns));
  AddBytes(&total, TextBytes(table.temporary_scope));
  AddBytes(&total, TextBytes(table.temporary_session_uuid));
  AddBytes(&total, TextBytes(table.on_commit_action));
  return total;
}

std::uint64_t RowVersionEstimateBytes(const CrudRowVersionRecord& row) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(row.table_uuid));
  AddBytes(&total, TextBytes(row.row_uuid));
  AddBytes(&total, TextBytes(row.version_uuid));
  AddBytes(&total, TextBytes(row.previous_version_uuid));
  AddBytes(&total, PairBytes(row.values));
  return total;
}

std::uint64_t IndexMetadataEstimateBytes(const CrudIndexRecord& index) {
  std::uint64_t total = 128;
  AddBytes(&total, TextBytes(index.index_uuid));
  AddBytes(&total, TextBytes(index.table_uuid));
  AddBytes(&total, TextBytes(index.column_name));
  AddBytes(&total, TextBytes(index.family));
  AddBytes(&total, TextBytes(index.profile));
  AddBytes(&total, TextBytes(index.default_name));
  AddBytes(&total, StringListBytes(index.key_envelopes));
  AddBytes(&total, StringListBytes(index.include_columns));
  AddBytes(&total, TextBytes(index.predicate_kind));
  AddBytes(&total, TextBytes(index.predicate_column));
  AddBytes(&total, TextBytes(index.predicate_value));
  return total;
}

std::uint64_t IndexEntryEstimateBytes(const CrudIndexEntryRecord& entry) {
  std::uint64_t total = 112;
  AddBytes(&total, TextBytes(entry.index_uuid));
  AddBytes(&total, TextBytes(entry.table_uuid));
  AddBytes(&total, TextBytes(entry.column_name));
  AddBytes(&total, TextBytes(entry.family));
  AddBytes(&total, TextBytes(entry.entry_kind));
  AddBytes(&total, TextBytes(entry.key_value));
  AddBytes(&total, TextBytes(entry.payload_value));
  AddBytes(&total, TextBytes(entry.row_uuid));
  AddBytes(&total, TextBytes(entry.version_uuid));
  return total;
}

bool TableUuidSeen(const std::vector<std::string>& seen, const std::string& table_uuid) {
  return std::find(seen.begin(), seen.end(), table_uuid) != seen.end();
}

MgaRelationStatistics EstimateRelationStatisticsFromState(const EngineRequestContext& context,
                                                          const CrudState& state,
                                                          const std::string& table_uuid,
                                                          bool include_indexes) {
  MgaRelationStatistics statistics;
  const auto table = FindVisibleCrudTable(state, table_uuid, context.local_transaction_id);
  if (!table) { return statistics; }

  statistics.relation_found = true;
  statistics.visible_row_estimate =
      static_cast<std::uint64_t>(VisibleCrudRowsForContext(state, table_uuid, context).size());

  // Stable estimate from persisted MGA relation sidecar payload lengths. This
  // is not page-byte accounting; it is engine-owned row-version/catalog size.
  AddBytes(&statistics.row_store_bytes, TableMetadataEstimateBytes(*table));
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    ++statistics.retained_row_version_count;
    AddBytes(&statistics.row_store_bytes, RowVersionEstimateBytes(row));
  }
  statistics.table_size_bytes = statistics.row_store_bytes;

  if (include_indexes) {
    const auto indexes = VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id);
    for (const auto& index : indexes) {
      AddBytes(&statistics.index_store_bytes, IndexMetadataEstimateBytes(index));
    }
    for (const auto& entry : state.index_entries) {
      if (entry.table_uuid != table_uuid) { continue; }
      AddBytes(&statistics.index_store_bytes, IndexEntryEstimateBytes(entry));
    }
    AddBytes(&statistics.table_size_bytes, statistics.index_store_bytes);
  }
  return statistics;
}

}  // namespace

MgaRelationStatisticsResult EstimateMgaRelationStatistics(const EngineRequestContext& context,
                                                          const std::string& table_uuid,
                                                          bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  result.statistics = EstimateRelationStatisticsFromState(context, state, table_uuid, include_indexes);
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

MgaRelationStatisticsResult EstimateMgaCatalogStatistics(const EngineRequestContext& context,
                                                         bool include_indexes) {
  MgaRelationStatisticsResult result;
  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  std::vector<std::string> table_uuids;
  for (const auto& table : state.tables) {
    if (table.table_uuid.empty() || TableUuidSeen(table_uuids, table.table_uuid)) { continue; }
    if (!FindVisibleCrudTable(state, table.table_uuid, context.local_transaction_id)) { continue; }
    table_uuids.push_back(table.table_uuid);
  }
  result.statistics.relation_found = true;
  for (const auto& table_uuid : table_uuids) {
    const auto table_stats = EstimateRelationStatisticsFromState(context, state, table_uuid, include_indexes);
    AddBytes(&result.statistics.visible_row_estimate, table_stats.visible_row_estimate);
    AddBytes(&result.statistics.retained_row_version_count, table_stats.retained_row_version_count);
    AddBytes(&result.statistics.row_store_bytes, table_stats.row_store_bytes);
    AddBytes(&result.statistics.index_store_bytes, table_stats.index_store_bytes);
    AddBytes(&result.statistics.table_size_bytes, table_stats.table_size_bytes);
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

EngineApiDiagnostic EnsureMgaRelationStorageDescriptor(const EngineRequestContext& context,
                                                       const CrudTableRecord& table,
                                                       const std::vector<CrudIndexRecord>& indexes,
                                                       MgaRelationStorageDescriptor* descriptor) {
  const auto persisted =
      LoadDescriptorFieldsByRelation(context, table.table_uuid);
  const auto existing = persisted.find(table.table_uuid);
  const auto fields = existing == persisted.end()
                          ? BuildPersistedMgaRelationDescriptorFields(context, table, indexes)
                          : existing->second;
  MgaRelationStorageDescriptor built =
      BuildMgaRelationStorageDescriptorFromCrudMetadata(context, table, indexes, fields);
  const auto validated = ValidateMgaRelationStorageDescriptor(built);
  if (validated.error) { return validated; }
  if (existing == persisted.end()) {
    const auto persisted_diagnostic = PersistDescriptorFields(context, table.table_uuid, fields);
    if (persisted_diagnostic.error) { return persisted_diagnostic; }
  }
  if (descriptor != nullptr) { *descriptor = std::move(built); }
  return OkDiagnostic();
}

MgaRelationStorageDescriptorLoadResult LoadMgaRelationStorageDescriptor(
    const EngineRequestContext& context,
    const std::string& relation_uuid) {
  MgaRelationStorageDescriptorLoadResult result;
  if (context.database_path.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "database_path_required");
    return result;
  }
  if (relation_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "relation_uuid_required");
    return result;
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load",
        "exact_active_transaction_identity_required");
    return result;
  }

  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "transaction_uuid_invalid");
    return result;
  }
  const auto inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!inventory.ok()) {
    result.diagnostic = MakeEngineApiDiagnostic(
        inventory.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : inventory.diagnostic.diagnostic_code,
        inventory.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : inventory.diagnostic.message_key,
        inventory.diagnostic.remediation_hint,
        true);
    return result;
  }
  const auto exact_transaction = LookupLocalTransaction(
      inventory.inventory,
      MakeLocalTransactionId(context.local_transaction_id));
  if (!exact_transaction.ok() ||
      exact_transaction.entry.identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      (exact_transaction.entry.state != TransactionState::active &&
       exact_transaction.entry.state != TransactionState::read_only_active)) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load",
        "exact_active_transaction_identity_required");
    return result;
  }

  CrudState metadata;
  const auto loaded = LoadMgaMetadata(&metadata, context);
  if (loaded.error) {
    result.diagnostic = loaded;
    return result;
  }
  for (const auto& entry : inventory.inventory.entries) {
    if (!entry.identity.local_id.valid()) { continue; }
    metadata.transactions[entry.identity.local_id.value] =
        MgaTransactionStateName(entry.state);
    metadata.max_transaction_id = std::max(
        metadata.max_transaction_id, entry.identity.local_id.value);
  }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  const auto table = FindVisibleCrudTable(
      metadata, relation_uuid, context.local_transaction_id);
  if (!table) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "relation_not_visible");
    return result;
  }

  const auto persisted =
      LoadDescriptorFieldsByRelation(context, relation_uuid);
  const auto fields = persisted.find(relation_uuid);
  if (fields == persisted.end()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "mga.relation_descriptor.load", "persisted_descriptor_required");
    return result;
  }
  const auto indexes = VisibleCrudIndexesForTable(
      metadata, relation_uuid, context.local_transaction_id);
  result.descriptor = BuildMgaRelationStorageDescriptorFromCrudMetadata(
      context, *table, indexes, fields->second);
  const auto validated =
      ValidateMgaRelationStorageDescriptor(result.descriptor);
  if (validated.error) {
    result.diagnostic = validated;
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
MgaVisibleHeapRelationReadResult ReadVisibleMgaHeapRelation(
    const EngineRequestContext& context,
    const MgaVisibleHeapRelationReadRequest& request) {
  MgaVisibleHeapRelationReadResult result;
  const auto refuse = [&](EngineApiDiagnostic diagnostic,
                          const BoundedScopedRowReadControl* control = nullptr) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.descriptor = {};
    result.visible_rows.clear();
    if (control != nullptr) {
      result.scanned_row_version_count = control->decoded_row_versions;
      result.decoded_byte_count = control->decoded_bytes;
      result.cancellation_observed = control->cancellation_observed;
    }
    return result;
  };
  const auto invalid = [&](std::string detail,
                           const BoundedScopedRowReadControl* control = nullptr) {
    return refuse(MakeInvalidRequestDiagnostic("mga.heap_relation_read",
                                               std::move(detail)),
                  control);
  };

  if (request.relation_uuid.empty()) {
    return invalid("relation_uuid_required");
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return invalid("exact_active_transaction_and_snapshot_required");
  }
  if (request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0) {
    return invalid("nonzero_heap_read_resource_bounds_required");
  }
  if (!request.cancellation_requested) {
    return invalid("engine_cancellation_probe_required");
  }
  if (request.cancellation_requested()) {
    result.cancellation_observed = true;
    return invalid("heap_read_cancelled_before_descriptor_load");
  }

  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = context;
  const auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok || !snapshot.snapshot_vector.inventory_authoritative ||
      !snapshot.snapshot_vector.complete) {
    return invalid("exact_current_statement_snapshot_vector_required");
  }

  const auto loaded_descriptor =
      LoadMgaRelationStorageDescriptor(context, request.relation_uuid);
  if (!loaded_descriptor.ok) { return refuse(loaded_descriptor.diagnostic); }
  const auto& descriptor = loaded_descriptor.descriptor;
  if (descriptor.relation_uuid.canonical != request.relation_uuid ||
      descriptor.database_uuid.canonical != context.database_uuid.canonical ||
      descriptor.relation_kind != "table" ||
      descriptor.storage_profile != "local_mga_rowstore_v1" ||
      descriptor.descriptor_uuid.canonical.empty() ||
      descriptor.descriptor_generation == 0 ||
      descriptor.descriptor_status.empty()) {
    return invalid("current_persisted_local_heap_descriptor_required");
  }

  CrudState metadata;
  const auto metadata_loaded = LoadMgaMetadata(&metadata, context);
  if (metadata_loaded.error) { return refuse(metadata_loaded); }
  const auto authority = OverlayMgaTransactionAuthority(context, &metadata);
  if (authority.error) { return refuse(authority); }
  FilterVisibleRetiredTemporaryMetadata(context, &metadata);
  FilterMgaTemporaryObjectsForSession(context, &metadata);
  const auto creator_visible = [&](const std::uint64_t creator) {
    const auto& vector = snapshot.snapshot_vector;
    if (creator == 0) return false;
    const auto transaction = metadata.transactions.find(creator);
    if (transaction == metadata.transactions.end()) return false;
    if (creator == vector.owning_transaction.value) {
      return transaction->second == "active" ||
             transaction->second == "preparing" ||
             transaction->second == "prepared";
    }
    if (transaction->second != "committed" &&
        transaction->second != "archived") {
      return false;
    }
    if (vector.visible_committed_high_watermark == 0 ||
        creator > vector.visible_committed_high_watermark) {
      return false;
    }
    return !std::binary_search(
               vector.active_excluded_local_transaction_ids.begin(),
               vector.active_excluded_local_transaction_ids.end(), creator) &&
           !std::binary_search(
               vector.in_doubt_excluded_local_transaction_ids.begin(),
               vector.in_doubt_excluded_local_transaction_ids.end(), creator);
  };
  const auto table = FindVisibleCrudTable(
      metadata, request.relation_uuid, context.local_transaction_id);
  if (!table) { return invalid("heap_relation_not_visible"); }
  if (table->temporary) {
    return invalid("temporary_relation_outside_local_heap_profile");
  }

  BoundedScopedRowReadControl control;
  control.maximum_row_versions = request.maximum_scanned_row_versions;
  control.maximum_bytes = request.maximum_decoded_bytes;
  control.cancellation_requested = request.cancellation_requested;
  std::vector<CrudRowVersionRecord> row_versions;
  bool used_segment = false;
  if (!LoadDecodedScopedRowsForTableBounded(context,
                                            request.relation_uuid,
                                            &control,
                                            &row_versions,
                                            &used_segment)) {
    return invalid(control.refusal_detail.empty()
                       ? "bounded_scoped_heap_read_failed"
                       : control.refusal_detail,
                   &control);
  }
  result.scanned_row_version_count = control.decoded_row_versions;
  result.decoded_byte_count = control.decoded_bytes;
  result.scoped_physical_segment_used = used_segment;

  const auto savepoints = ParseSavepoints(context);
  std::vector<CrudRowVersionRecord> admitted_versions;
  admitted_versions.reserve(row_versions.size());
  for (auto& row : row_versions) {
    if (request.cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    if (row.table_uuid != request.relation_uuid) {
      return invalid("scoped_heap_row_relation_identity_mismatch", &control);
    }
    if (RowEventRolledBackBySavepoint(savepoints,
                                      row.creator_tx,
                                      row.event_sequence)) {
      ++result.invisible_row_version_count;
      continue;
    }
    admitted_versions.push_back(std::move(row));
  }
  const auto chain_status =
      ValidateMgaRowVersionRecordChains(admitted_versions);
  if (chain_status.error) { return refuse(chain_status, &control); }
  if (RowsContainLargeValueLocators(admitted_versions)) {
    return invalid("large_value_outside_bounded_inline_heap_profile",
                   &control);
  }

  std::unordered_map<std::string, std::size_t> newest_visible_by_row;
  newest_visible_by_row.reserve(admitted_versions.size());
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (request.cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_visibility";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    ++result.visibility_recheck_count;
    if ((!row.temporary_session_uuid.empty() &&
         row.temporary_session_uuid != context.session_uuid.canonical) ||
        !creator_visible(row.creator_tx)) {
      ++result.invisible_row_version_count;
      continue;
    }
    const auto found = newest_visible_by_row.find(row.row_uuid);
    if (found == newest_visible_by_row.end() ||
        admitted_versions[found->second].sequence < row.sequence) {
      newest_visible_by_row[row.row_uuid] = index;
    }
  }

  std::vector<CrudRowVersionRecord> visible_rows;
  visible_rows.reserve(newest_visible_by_row.size());
  for (std::size_t index = 0; index < admitted_versions.size(); ++index) {
    if (request.cancellation_requested()) {
      control.cancellation_observed = true;
      control.refusal_detail = "heap_read_cancelled_during_publication";
      return invalid(control.refusal_detail, &control);
    }
    const auto& row = admitted_versions[index];
    const auto newest = newest_visible_by_row.find(row.row_uuid);
    if (newest == newest_visible_by_row.end() || newest->second != index) {
      continue;
    }
    if (row.deleted) {
      ++result.tombstone_row_count;
      continue;
    }
    if (visible_rows.size() >= request.maximum_output_rows) {
      return invalid("heap_read_maximum_output_rows_exceeded", &control);
    }
    visible_rows.push_back(row);
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.descriptor = descriptor;
  result.visible_rows = std::move(visible_rows);
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_uuid",
       descriptor.descriptor_uuid.canonical});
  result.evidence.push_back(
      {"mga_heap_read_relation_descriptor_generation",
       std::to_string(descriptor.descriptor_generation)});
  result.evidence.push_back(
      {"mga_heap_read_storage_route", "bounded_scoped_physical_segment"});
  result.evidence.push_back(
      {"mga_heap_read_visibility_authority",
       "durable_transaction_inventory_statement_snapshot"});
  result.evidence.push_back(
      {"mga_heap_read_parser_or_candidate_authority", "false"});
  return result;
}

EngineApiDiagnostic AppendMgaRowVersion(const EngineRequestContext& context,
                                         const CrudRowVersionRecord& row,
                                         std::uint64_t* written_event_sequence) {
  std::vector<CrudRowVersionRecord> rows{row};
  std::vector<std::uint64_t> sequences;
  const auto diagnostic = AppendMgaRowVersions(context, &rows, written_event_sequence == nullptr ? nullptr : &sequences);
  if (diagnostic.error) { return diagnostic; }
  if (written_event_sequence != nullptr && !sequences.empty()) {
    *written_event_sequence = sequences.front();
  }
  return OkDiagnostic();
}

struct MgaRelationHotAppendContext::Impl {
  explicit Impl(const EngineRequestContext& source_context) : context(source_context) {}

  EngineRequestContext context;
  std::ofstream row_out;
  std::ofstream index_out;
  std::vector<std::string> allocator_lines;
  std::map<std::string, std::string> scoped_row_lines;
  std::map<std::string, std::string> scoped_row_binary_buffers;
  std::map<std::string, std::string> scoped_index_lines;
  std::map<std::string, std::string> scoped_index_binary_buffers;
  std::map<std::string, std::vector<CrudRowVersionRecord>>
      scoped_decoded_row_appends;
  std::vector<PreparedIndexAppendJob> pending_prepared_index_jobs;
  std::vector<std::future<PreparedIndexAppendJob>> pending_index_materialization_jobs;
  std::map<std::string, ScopedRelationSummaryDelta> scoped_row_summary_deltas;
  bool decoded_row_cache_auto_warm = true;
  bool row_dirty = false;
  bool index_dirty = false;
  MgaRelationHotAppendCounters counters;
};

MgaRelationHotAppendContext::MgaRelationHotAppendContext(
    const EngineRequestContext& context)
    : impl_(std::make_unique<Impl>(context)) {}

MgaRelationHotAppendContext::~MgaRelationHotAppendContext() = default;

MgaRelationHotAppendContext::MgaRelationHotAppendContext(
    MgaRelationHotAppendContext&&) noexcept = default;

MgaRelationHotAppendContext& MgaRelationHotAppendContext::operator=(
    MgaRelationHotAppendContext&&) noexcept = default;

const MgaRelationHotAppendCounters& MgaRelationHotAppendContext::counters() const {
  return impl_->counters;
}

void MgaRelationHotAppendContext::SetDecodedRowCacheAutoWarm(bool enabled) {
  impl_->decoded_row_cache_auto_warm = enabled;
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersions(
    std::vector<CrudRowVersionRecord>* rows,
    std::vector<std::uint64_t>* written_event_sequences) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows == nullptr || rows->empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  if (written_event_sequences != nullptr) {
    written_event_sequences->clear();
    written_event_sequences->reserve(rows->size());
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows->size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows->size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows->front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows->begin()), rows->end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows->size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows->size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows->size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : *rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (auto& writable : *rows) {
    writable.event_sequence = event_sequence++;
    writable.sequence = writable.event_sequence;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer, writable);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(writable.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    if (single_table_batch) {
      if (single_decoded_appends != nullptr) {
        single_decoded_appends->push_back(writable);
      }
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded->second.push_back(writable);
      }
    }
    auto& summary_delta = single_table_batch
                              ? *single_summary_delta
                              : impl_->scoped_row_summary_deltas[writable.table_uuid];
    ++summary_delta.row_version_count;
    if (writable.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!writable.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
    if (written_event_sequences != nullptr) {
      written_event_sequences->push_back(writable.event_sequence);
    }
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersions(
    std::vector<CrudRowVersionRecord>* rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch,
    std::vector<std::uint64_t>* written_event_sequences) {
  if (value_batch == nullptr) {
    return AppendRowVersions(rows, written_event_sequences);
  }
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows == nullptr || rows->empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch->size() != rows->size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context),
                        std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows->size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows->size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows->front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows->begin()), rows->end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows->size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows->size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows->size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : *rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (std::size_t index = 0; index < rows->size(); ++index) {
    auto& writable = (*rows)[index];
    const auto& values = (*value_batch)[index];
    writable.event_sequence = event_sequence++;
    writable.sequence = writable.event_sequence;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer,
                              writable,
                              writable.event_sequence,
                              values);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(writable.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = writable;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta = single_table_batch
                              ? *single_summary_delta
                              : impl_->scoped_row_summary_deltas[writable.table_uuid];
    ++summary_delta.row_version_count;
    if (writable.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!writable.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
    if (written_event_sequences != nullptr) {
      written_event_sequences->push_back(writable.event_sequence);
    }
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersionsReadOnly(
    const std::vector<CrudRowVersionRecord>& rows) {
  return AppendRowVersionsReadOnly(
      rows,
      static_cast<const std::vector<std::vector<std::pair<std::string, std::string>>>*>(
          nullptr));
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendRowVersionsReadOnly(
    const std::vector<CrudRowVersionRecord>& rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch != nullptr && value_batch->size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  if (!impl_->row_out.is_open()) {
    impl_->row_out.open(RowStorePath(impl_->context), std::ios::app | std::ios::binary);
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store", "row_version_append_failed");
    }
    ++impl_->counters.row_stream_opens;
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::string row_buffer;
  row_buffer.reserve(rows.size() * kHotAppendRowLineReserveBytes);
  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows.size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows.size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows.size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    const auto& values =
        value_batch == nullptr ? row.values : (*value_batch)[index];
    const std::uint64_t row_event_sequence = event_sequence++;
    const std::size_t line_start = row_buffer.size();
    AppendRowVersionStoreLine(&row_buffer, row, row_event_sequence, values);
    row_buffer.push_back('\n');
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(row.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    scoped_buffer.append(row_buffer.data() + line_start,
                         row_buffer.size() - line_start);
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = row;
      cache_row.event_sequence = row_event_sequence;
      cache_row.sequence = row_event_sequence;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta =
        single_table_batch ? *single_summary_delta
                           : impl_->scoped_row_summary_deltas[row.table_uuid];
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    impl_->row_dirty = true;
    ++impl_->counters.row_versions_appended;
  }
  if (!row_buffer.empty()) {
    impl_->row_out.write(row_buffer.data(),
                         static_cast<std::streamsize>(row_buffer.size()));
    if (!impl_->row_out) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "row_version_append_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionsReadOnlyScopedOnly(
    const std::vector<CrudRowVersionRecord>& rows,
    const std::vector<std::vector<std::pair<std::string, std::string>>>*
        value_batch,
    bool shared_key_order_known) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (value_batch != nullptr && value_batch->size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_value_batch_shape_invalid");
  }
  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;
  std::uint64_t event_sequence = reservation.first;
  std::vector<std::string> encoded_key_cache;
  const auto row_values = [&](std::size_t index)
      -> const std::vector<std::pair<std::string, std::string>>& {
    return value_batch == nullptr ? rows[index].values : (*value_batch)[index];
  };
  if (!rows.empty()) {
    const auto& first_values = row_values(0);
    bool shared_key_order = !first_values.empty();
    if (!shared_key_order_known) {
      for (std::size_t row_index = 1;
           shared_key_order && row_index < rows.size();
           ++row_index) {
        const auto& values = row_values(row_index);
        if (values.size() != first_values.size()) {
          shared_key_order = false;
          break;
        }
        for (std::size_t field_index = 0; field_index < values.size();
             ++field_index) {
          if (values[field_index].first != first_values[field_index].first) {
            shared_key_order = false;
            break;
          }
        }
      }
    }
    if (shared_key_order) {
      encoded_key_cache.reserve(first_values.size());
      for (const auto& [key, _] : first_values) {
        std::string encoded_key;
        encoded_key.reserve(key.size() * 2);
        AppendHexEncoded(&encoded_key, key);
        encoded_key_cache.push_back(std::move(encoded_key));
      }
    }
  }
  const auto* encoded_key_cache_ptr =
      encoded_key_cache.empty() ? nullptr : &encoded_key_cache;

  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  std::string single_scoped_path;
  std::string* single_scoped_buffer = nullptr;
  std::vector<CrudRowVersionRecord>* single_decoded_appends = nullptr;
  ScopedRelationSummaryDelta* single_summary_delta = nullptr;
  std::map<std::string, std::size_t> rows_per_table;
  std::map<std::string, std::string> scoped_row_path_by_table;
  if (single_table_batch) {
    single_scoped_path = ScopedRowStorePath(impl_->context, single_table_uuid);
    single_scoped_buffer = &impl_->scoped_row_lines[single_scoped_path];
    single_scoped_buffer->reserve(
        single_scoped_buffer->size() +
        rows.size() * kHotAppendRowLineReserveBytes);
    if (impl_->decoded_row_cache_auto_warm &&
        rows.size() <= kScopedDecodedRowCacheMaxAutoWarmRows) {
      single_decoded_appends =
          &impl_->scoped_decoded_row_appends[single_scoped_path];
      single_decoded_appends->reserve(single_decoded_appends->size() +
                                      rows.size());
    }
    single_summary_delta = &impl_->scoped_row_summary_deltas[single_table_uuid];
    auto& summary_delta = *single_summary_delta;
    if (summary_delta.row_version_count == 0 &&
        summary_delta.tombstone_count == 0 &&
        summary_delta.update_count == 0) {
      summary_delta.first_scoped_write =
          !ScopedRelationAnyRowStoreExists(impl_->context,
                                           single_table_uuid) &&
          !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                        single_table_uuid));
    }
  } else {
    for (const auto& row : rows) {
      ++rows_per_table[row.table_uuid];
    }
    for (const auto& [table_uuid, row_count] : rows_per_table) {
      const std::string scoped_path = ScopedRowStorePath(impl_->context, table_uuid);
      std::string& scoped_buffer = impl_->scoped_row_lines[scoped_path];
      scoped_buffer.reserve(scoped_buffer.size() +
                            row_count * kHotAppendRowLineReserveBytes);
      scoped_row_path_by_table.emplace(table_uuid, scoped_path);
      if (impl_->decoded_row_cache_auto_warm &&
          row_count <= kScopedDecodedRowCacheMaxAutoWarmRows) {
        auto& decoded_appends = impl_->scoped_decoded_row_appends[scoped_path];
        decoded_appends.reserve(decoded_appends.size() + row_count);
      }
      auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
      if (summary_delta.row_version_count == 0 &&
          summary_delta.tombstone_count == 0 &&
          summary_delta.update_count == 0) {
        summary_delta.first_scoped_write =
            !ScopedRelationAnyRowStoreExists(impl_->context, table_uuid) &&
            !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                          table_uuid));
      }
    }
  }

  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    const auto& values = row_values(index);
    const std::uint64_t row_event_sequence = event_sequence++;
    const std::string& scoped_path =
        single_table_batch
            ? single_scoped_path
            : scoped_row_path_by_table.find(row.table_uuid)->second;
    std::string& scoped_buffer =
        single_table_batch ? *single_scoped_buffer
                           : impl_->scoped_row_lines[scoped_path];
    AppendRowVersionStoreLine(&scoped_buffer,
                              row,
                              row_event_sequence,
                              values,
                              encoded_key_cache_ptr);
    scoped_buffer.push_back('\n');
    std::vector<CrudRowVersionRecord>* decoded_appends = nullptr;
    if (single_table_batch) {
      decoded_appends = single_decoded_appends;
    } else {
      auto decoded = impl_->scoped_decoded_row_appends.find(scoped_path);
      if (decoded != impl_->scoped_decoded_row_appends.end()) {
        decoded_appends = &decoded->second;
      }
    }
    if (decoded_appends != nullptr) {
      CrudRowVersionRecord cache_row = row;
      cache_row.event_sequence = row_event_sequence;
      cache_row.sequence = row_event_sequence;
      cache_row.values = values;
      decoded_appends->push_back(std::move(cache_row));
    }
    auto& summary_delta =
        single_table_batch ? *single_summary_delta
                           : impl_->scoped_row_summary_deltas[row.table_uuid];
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    ++impl_->counters.row_versions_appended;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionsReadOnlyScopedOnlyTyped(
    const std::vector<CrudRowVersionRecord>& rows,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> shared_field_order) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (rows.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (typed_rows.size() != rows.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_value_batch_shape_invalid");
  }
  if (shared_field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_field_order_required");
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(rows.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string single_table_uuid = rows.front().table_uuid;
  const bool single_table_batch =
      std::all_of(std::next(rows.begin()), rows.end(), [&](const auto& row) {
        return row.table_uuid == single_table_uuid;
      });
  if (!single_table_batch) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_single_table_required");
  }

  for (const auto& typed_row : typed_rows) {
    if (typed_row.fields.size() != shared_field_order.size()) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_field_order_mismatch");
    }
  }

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         single_table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(
      impl_->context,
      single_table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[single_table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      single_table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowBinaryBatch(&scoped_buffer,
                                  rows,
                                  typed_rows,
                                  shared_field_order,
                                  reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(rows.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);

  for (const auto& row : rows) {
    ++summary_delta.row_version_count;
    if (row.deleted) {
      ++summary_delta.tombstone_count;
    }
    if (!row.previous_version_uuid.empty()) {
      ++summary_delta.update_count;
    }
    ++impl_->counters.row_versions_appended;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionIdentitiesReadOnlyScopedOnlyTyped(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    std::span<const EngineRowValue> typed_rows,
    std::span<const std::string> shared_field_order) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (row_identities.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (table_uuid.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "target_table_uuid_required");
  }
  if (table_uuid == "unknown") {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "target_table_uuid_unresolved");
  }
  if (typed_rows.size() != row_identities.size()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_value_batch_shape_invalid");
  }
  if (shared_field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_field_order_required");
  }
  for (const auto& typed_row : typed_rows) {
    if (typed_row.fields.size() != shared_field_order.size()) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_field_order_mismatch");
    }
  }
  for (const auto& row : row_identities) {
    if (!row.table_uuid.empty() && row.table_uuid != table_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "typed_row_table_uuid_mismatch");
    }
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(row_identities.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(impl_->context,
                                                                 table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowIdentityBinaryBatch(&scoped_buffer,
                                          row_identities,
                                          table_uuid,
                                          temporary_session_uuid,
                                          typed_rows,
                                          shared_field_order,
                                          impl_->context.local_transaction_id,
                                          reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "typed_row_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);
  summary_delta.row_version_count +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.row_versions_appended +=
      static_cast<std::uint64_t>(row_identities.size());
  return OkDiagnostic();
}

EngineApiDiagnostic
MgaRelationHotAppendContext::AppendRowVersionIdentitiesReadOnlyScopedOnlyNativePacket(
    const std::vector<CrudRowVersionRecord>& row_identities,
    const std::string& table_uuid,
    const std::string& temporary_session_uuid,
    const EngineNativeRowPacketFrame& frame) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "database_path_required");
  }
  if (row_identities.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store", "row_versions_required");
  }
  if (table_uuid.empty() || table_uuid == "unknown") {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "target_table_uuid_unresolved");
  }
  if (!frame.present || frame.row_count != row_identities.size() ||
      frame.field_order.empty()) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "native_row_packet_shape_invalid");
  }
  for (const auto& row : row_identities) {
    if (!row.table_uuid.empty() && row.table_uuid != table_uuid) {
      return MakeInvalidRequestDiagnostic("mga.row_store",
                                          "native_row_packet_table_uuid_mismatch");
    }
  }

  const auto reservation = ReserveEventSequenceRange(
      impl_->context,
      "row_versions",
      RowStorePath(impl_->context),
      static_cast<std::uint64_t>(row_identities.size()),
      [this]() { return ScanNextRowEventSequence(impl_->context); },
      &impl_->allocator_lines);
  if (!reservation.ok) { return reservation.diagnostic; }
  ++impl_->counters.row_range_reservations;

  const std::string scoped_text_path = ScopedRowStorePath(impl_->context,
                                                         table_uuid);
  const std::string scoped_binary_path = ScopedRowBinaryStorePath(impl_->context,
                                                                 table_uuid);
  std::string& scoped_buffer =
      impl_->scoped_row_binary_buffers[scoped_binary_path];
  auto& summary_delta = impl_->scoped_row_summary_deltas[table_uuid];
  if (summary_delta.row_version_count == 0 &&
      summary_delta.tombstone_count == 0 &&
      summary_delta.update_count == 0) {
    summary_delta.first_scoped_write =
        !FileExistsAndNotEmpty(scoped_text_path) &&
        !FileExistsAndNotEmpty(scoped_binary_path) &&
        !FileExistsAndNotEmpty(ScopedSummaryStorePath(impl_->context,
                                                      table_uuid));
  }

  const std::size_t binary_buffer_start = scoped_buffer.size();
  if (!AppendScopedRowIdentityNativePacketBatch(&scoped_buffer,
                                                row_identities,
                                                table_uuid,
                                                temporary_session_uuid,
                                                frame,
                                                impl_->context.local_transaction_id,
                                                reservation.first)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "native_row_packet_binary_batch_encode_failed");
  }
  ++impl_->counters.scoped_row_binary_batches;
  impl_->counters.scoped_row_binary_rows +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.scoped_row_binary_bytes +=
      static_cast<std::uint64_t>(scoped_buffer.size() - binary_buffer_start);
  summary_delta.row_version_count +=
      static_cast<std::uint64_t>(row_identities.size());
  impl_->counters.row_versions_appended +=
      static_cast<std::uint64_t>(row_identities.size());
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushRowVersions() {
  if (!AppendDeferredEventSequenceAllocatorLines(impl_->context,
                                                 &impl_->allocator_lines,
                                                 &impl_->counters)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "event_sequence_allocator_batch_append_failed");
  }
  const bool has_scoped_rows =
      !impl_->scoped_row_lines.empty() ||
      !impl_->scoped_row_binary_buffers.empty();
  std::map<std::string, std::string> scoped_row_write_buffers =
      std::move(impl_->scoped_row_lines);
  for (auto& [path, buffer] : impl_->scoped_row_binary_buffers) {
    auto found = scoped_row_write_buffers.find(path);
    if (found != scoped_row_write_buffers.end()) {
      found->second.append(buffer);
    } else {
      scoped_row_write_buffers.emplace(std::move(path), std::move(buffer));
    }
  }
  impl_->scoped_row_binary_buffers.clear();
  const bool can_overlap_scoped_rows =
      has_scoped_rows && impl_->row_out.is_open() && impl_->row_dirty;
  std::future<ScopedRelationAppendResult> scoped_row_future;
  if (can_overlap_scoped_rows) {
    scoped_row_future = std::async(
        std::launch::async,
        [pending = &scoped_row_write_buffers]() {
          return AppendScopedRelationLinesWithCounters(*pending);
        });
  }
  bool row_flush_ok = true;
  if (impl_->row_out.is_open() && impl_->row_dirty) {
    impl_->row_out.flush();
    if (!impl_->row_out) {
      row_flush_ok = false;
    } else {
      impl_->row_dirty = false;
      ++impl_->counters.row_stream_flushes;
    }
  }
  ScopedRelationAppendResult scoped_row_result;
  scoped_row_result.ok = true;
  if (can_overlap_scoped_rows) {
    scoped_row_result = scoped_row_future.get();
    AddScopedRelationAppendCounters(
        scoped_row_result,
        &impl_->counters.scoped_row_stream_opens,
        &impl_->counters.scoped_row_stream_flushes,
        &impl_->counters.scoped_row_write_batches,
        &impl_->counters.scoped_row_write_tickets_issued,
        &impl_->counters.scoped_row_write_tickets_completed,
        &impl_->counters.scoped_row_write_worker_count);
  } else if (has_scoped_rows) {
    scoped_row_result = AppendScopedRelationLinesWithCounters(
        scoped_row_write_buffers);
    AddScopedRelationAppendCounters(
        scoped_row_result,
        &impl_->counters.scoped_row_stream_opens,
        &impl_->counters.scoped_row_stream_flushes,
        &impl_->counters.scoped_row_write_batches,
        &impl_->counters.scoped_row_write_tickets_issued,
        &impl_->counters.scoped_row_write_tickets_completed,
        &impl_->counters.scoped_row_write_worker_count);
  }
  if (!row_flush_ok) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "row_version_append_failed");
  }
  if (!scoped_row_result.ok) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "scoped_row_version_append_failed");
  }
  if (!UpdateScopedRelationSummaries(impl_->context,
                                     impl_->scoped_row_summary_deltas)) {
    return MakeInvalidRequestDiagnostic("mga.row_store",
                                        "scoped_relation_summary_update_failed");
  }
  if (!impl_->scoped_row_binary_buffers.empty()) {
    const std::lock_guard<std::mutex> guard(ScopedDecodedRowCacheMutex());
    ScopedDecodedRowCache().clear();
  } else {
    UpdateScopedDecodedRowCacheAfterAppend(impl_->scoped_decoded_row_appends,
                                           impl_->scoped_row_lines);
  }
  impl_->scoped_row_lines.clear();
  impl_->scoped_row_binary_buffers.clear();
  impl_->scoped_decoded_row_appends.clear();
  impl_->scoped_row_summary_deltas.clear();
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendIndexEntryBatches(
    const std::vector<MgaIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  const bool inline_materialization = batches.size() <= 1;
  for (const auto& batch : batches) {
    if (batch.rows.empty()) { continue; }
    try {
      ++impl_->counters.index_materialization_jobs_queued;
      if (inline_materialization) {
        PreparedIndexAppendJob job =
            BuildPreparedIndexAppendJob(batch);
        if (!job.ok) { return job.diagnostic; }
        impl_->pending_prepared_index_jobs.push_back(std::move(job));
        ++impl_->counters.index_materialization_inline_jobs;
      } else {
        std::vector<MgaIndexEntryAppendBatch> job_batches;
        job_batches.push_back(batch);
        impl_->pending_index_materialization_jobs.push_back(
            std::async(std::launch::async,
                       [job_batches = std::move(job_batches)]() mutable {
                         return BuildPreparedIndexAppendJob(std::move(job_batches));
                       }));
        impl_->counters.index_materialization_worker_count =
            std::max<std::uint64_t>(
                impl_->counters.index_materialization_worker_count,
                static_cast<std::uint64_t>(
                    impl_->pending_index_materialization_jobs.size()));
      }
    } catch (const std::exception& ex) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          std::string("index_materialization_enqueue_failed:") +
                                              ex.what());
    } catch (...) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "index_materialization_enqueue_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::AppendExactIndexEntryBatches(
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  if (impl_->context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.index_store", "database_path_required");
  }
  if (batches.size() == 1 && impl_->pending_prepared_index_jobs.empty() &&
      impl_->pending_index_materialization_jobs.empty() &&
      ExactIndexBatchAlreadyInAppendOrder(batches.front())) {
    const auto& batch = batches.front();
    if (batch.entries.empty()) {
      return OkDiagnostic();
    }
    const std::string table_uuid =
        batch.index.table_uuid.empty() ? batch.table_uuid : batch.index.table_uuid;
    if (table_uuid.empty() || batch.index.index_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_entry_invalid");
    }
    for (const auto& entry : batch.entries) {
      if (entry.encoded_key.empty() || entry.row_uuid.empty() ||
          entry.version_uuid.empty()) {
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            "exact_index_entry_invalid");
      }
    }

    const auto reservation = ReserveEventSequenceRange(
        impl_->context,
        "index_entries",
        IndexStorePath(impl_->context),
        static_cast<std::uint64_t>(batch.entries.size()),
        [this]() { return ScanNextIndexEventSequence(impl_->context); },
        &impl_->allocator_lines);
    if (!reservation.ok) { return reservation.diagnostic; }
    ++impl_->counters.index_range_reservations;

    const std::string scoped_path = ScopedIndexBinaryStorePath(impl_->context,
                                                              table_uuid);
    std::string& scoped_buffer =
        impl_->scoped_index_binary_buffers[scoped_path];
    if (!AppendScopedExactIndexBinaryBatch(&scoped_buffer,
                                           batch,
                                           impl_->context.local_transaction_id,
                                           reservation.first)) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_binary_batch_encode_failed");
    }

    ++impl_->counters.index_materialization_jobs_queued;
    ++impl_->counters.index_materialization_jobs_completed;
    ++impl_->counters.index_materialization_inline_jobs;
    impl_->counters.index_materialized_entries +=
        static_cast<std::uint64_t>(batch.entries.size());
    impl_->counters.index_entries_appended +=
        static_cast<std::uint64_t>(batch.entries.size());
    return OkDiagnostic();
  }
  const bool inline_materialization = batches.size() <= 1;
  for (const auto& batch : batches) {
    if (batch.entries.empty()) { continue; }
    for (const auto& entry : batch.entries) {
      if (entry.encoded_key.empty() || entry.row_uuid.empty() || entry.version_uuid.empty()) {
        return MakeInvalidRequestDiagnostic("mga.index_store", "exact_index_entry_invalid");
      }
    }
    try {
      ++impl_->counters.index_materialization_jobs_queued;
      if (inline_materialization) {
        PreparedIndexAppendJob job =
            BuildPreparedExactIndexAppendJob(batch);
        if (!job.ok) { return job.diagnostic; }
        impl_->pending_prepared_index_jobs.push_back(std::move(job));
        ++impl_->counters.index_materialization_inline_jobs;
      } else {
        std::vector<MgaExactIndexEntryAppendBatch> job_batches;
        job_batches.push_back(batch);
        impl_->pending_index_materialization_jobs.push_back(
            std::async(std::launch::async,
                       [job_batches = std::move(job_batches)]() mutable {
                         return BuildPreparedExactIndexAppendJob(std::move(job_batches));
                       }));
        impl_->counters.index_materialization_worker_count =
            std::max<std::uint64_t>(
                impl_->counters.index_materialization_worker_count,
                static_cast<std::uint64_t>(
                    impl_->pending_index_materialization_jobs.size()));
      }
    } catch (const std::exception& ex) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          std::string("exact_index_materialization_enqueue_failed:") +
                                              ex.what());
    } catch (...) {
      return MakeInvalidRequestDiagnostic("mga.index_store",
                                          "exact_index_materialization_enqueue_failed");
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::FlushIndexEntries() {
  if (!impl_->pending_prepared_index_jobs.empty() ||
      !impl_->pending_index_materialization_jobs.empty()) {
    std::vector<PreparedIndexAppendJob> prepared_jobs;
    prepared_jobs.reserve(impl_->pending_prepared_index_jobs.size() +
                          impl_->pending_index_materialization_jobs.size());
    std::uint64_t entry_count = 0;
    for (auto& job : impl_->pending_prepared_index_jobs) {
      ++impl_->counters.index_materialization_jobs_completed;
      if (!job.ok) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return job.diagnostic;
      }
      entry_count += static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialized_entries +=
          static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialization_sort_batches +=
          job.sorted_batch_count;
      prepared_jobs.push_back(std::move(job));
    }
    impl_->pending_prepared_index_jobs.clear();
    for (auto& future : impl_->pending_index_materialization_jobs) {
      PreparedIndexAppendJob job;
      try {
        job = future.get();
      } catch (const std::exception& ex) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            std::string("index_materialization_worker_failed:") +
                                                ex.what());
      } catch (...) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return MakeInvalidRequestDiagnostic("mga.index_store",
                                            "index_materialization_worker_failed");
      }
      ++impl_->counters.index_materialization_jobs_completed;
      if (!job.ok) {
        impl_->pending_prepared_index_jobs.clear();
        impl_->pending_index_materialization_jobs.clear();
        return job.diagnostic;
      }
      entry_count += static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialized_entries +=
          static_cast<std::uint64_t>(job.entries.size());
      impl_->counters.index_materialization_sort_batches +=
          job.sorted_batch_count;
      prepared_jobs.push_back(std::move(job));
    }
    impl_->pending_index_materialization_jobs.clear();

    if (entry_count != 0) {
      const auto reservation = ReserveEventSequenceRange(
          impl_->context,
          "index_entries",
          IndexStorePath(impl_->context),
          entry_count,
          [this]() { return ScanNextIndexEventSequence(impl_->context); },
          &impl_->allocator_lines);
      if (!reservation.ok) { return reservation.diagnostic; }
      ++impl_->counters.index_range_reservations;

      std::vector<std::future<PreparedIndexLineBufferJob>> line_futures;
      line_futures.reserve(prepared_jobs.size());
      std::uint64_t event_sequence = reservation.first;
      for (auto& job : prepared_jobs) {
        if (job.entries.empty()) { continue; }
        const std::uint64_t first_sequence = event_sequence;
        event_sequence += static_cast<std::uint64_t>(job.entries.size());
        if (prepared_jobs.size() == 1) {
          PreparedIndexLineBufferJob buffer_job =
              BuildPreparedIndexLineBuffers(impl_->context,
                                            std::move(job.entries),
                                            first_sequence);
          if (!buffer_job.ok) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                "index_line_buffer_worker_refused");
          }
          for (auto& [path, buffer] : buffer_job.scoped_lines) {
            impl_->scoped_index_lines[path].append(buffer);
          }
        } else {
          try {
            line_futures.push_back(
                std::async(std::launch::async,
                           [context = impl_->context,
                            entries = std::move(job.entries),
                            first_sequence]() mutable {
                             return BuildPreparedIndexLineBuffers(context,
                                                                  std::move(entries),
                                                                  first_sequence);
                           }));
          } catch (const std::exception& ex) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                std::string("index_line_buffer_enqueue_failed:") +
                                                    ex.what());
          } catch (...) {
            return MakeInvalidRequestDiagnostic("mga.index_store",
                                                "index_line_buffer_enqueue_failed");
          }
        }
      }
      impl_->counters.index_materialization_worker_count =
          std::max<std::uint64_t>(
              impl_->counters.index_materialization_worker_count,
              static_cast<std::uint64_t>(line_futures.size()));

      for (auto& future : line_futures) {
        PreparedIndexLineBufferJob buffer_job;
        try {
          buffer_job = future.get();
        } catch (const std::exception& ex) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              std::string("index_line_buffer_worker_failed:") +
                                                  ex.what());
        } catch (...) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              "index_line_buffer_worker_failed");
        }
        if (!buffer_job.ok) {
          return MakeInvalidRequestDiagnostic("mga.index_store",
                                              "index_line_buffer_worker_refused");
        }
        for (auto& [path, buffer] : buffer_job.scoped_lines) {
          impl_->scoped_index_lines[path].append(buffer);
        }
      }
      impl_->counters.index_entries_appended += entry_count;
      impl_->index_dirty = true;
    }
  }
  if (!AppendDeferredEventSequenceAllocatorLines(impl_->context,
                                                 &impl_->allocator_lines,
                                                 &impl_->counters)) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "event_sequence_allocator_batch_append_failed");
  }
  bool index_flush_ok = true;
  if (impl_->index_out.is_open() && impl_->index_dirty) {
    impl_->index_out.flush();
    if (!impl_->index_out) {
      index_flush_ok = false;
    } else {
      impl_->index_dirty = false;
      ++impl_->counters.index_stream_flushes;
    }
  }
  ScopedRelationAppendResult scoped_index_result;
  scoped_index_result.ok = true;
  const bool has_scoped_indexes =
      !impl_->scoped_index_lines.empty() ||
      !impl_->scoped_index_binary_buffers.empty();
  std::map<std::string, std::string> scoped_index_write_buffers =
      std::move(impl_->scoped_index_lines);
  for (auto& [path, buffer] : impl_->scoped_index_binary_buffers) {
    auto found = scoped_index_write_buffers.find(path);
    if (found != scoped_index_write_buffers.end()) {
      found->second.append(buffer);
    } else {
      scoped_index_write_buffers.emplace(std::move(path), std::move(buffer));
    }
  }
  impl_->scoped_index_binary_buffers.clear();
  if (has_scoped_indexes) {
    scoped_index_result = AppendScopedRelationLinesWithCounters(
        scoped_index_write_buffers);
    AddScopedRelationAppendCounters(
        scoped_index_result,
        &impl_->counters.scoped_index_stream_opens,
        &impl_->counters.scoped_index_stream_flushes,
        &impl_->counters.scoped_index_write_batches,
        &impl_->counters.scoped_index_write_tickets_issued,
        &impl_->counters.scoped_index_write_tickets_completed,
        &impl_->counters.scoped_index_write_worker_count);
  }
  if (!index_flush_ok) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "index_entry_append_failed");
  }
  if (!scoped_index_result.ok) {
    return MakeInvalidRequestDiagnostic("mga.index_store",
                                        "scoped_index_entry_append_failed");
  }
  impl_->scoped_index_lines.clear();
  impl_->scoped_index_binary_buffers.clear();
  return OkDiagnostic();
}

EngineApiDiagnostic MgaRelationHotAppendContext::Flush() {
  const auto rows = FlushRowVersions();
  if (rows.error) { return rows; }
  return FlushIndexEntries();
}

EngineApiDiagnostic AppendMgaRowVersions(const EngineRequestContext& context,
                                          std::vector<CrudRowVersionRecord>* rows,
                                          std::vector<std::uint64_t>* written_event_sequences) {
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendRowVersions(rows, written_event_sequences);
  if (appended.error) { return appended; }
  return append_context.FlushRowVersions();
}

EngineApiDiagnostic AppendMgaTableMetadata(const EngineRequestContext& context,
                                           const CrudTableRecord& table) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(context, "mga.relation_metadata.table_create");
  if (authority.error) { return authority; }
  CrudTableRecord writable = table;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "TABLE_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.table_uuid,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudPairs(writable.columns),
                                     writable.temporary ? "1" : "0",
                                     writable.temporary_scope,
                                     writable.temporary_session_uuid,
                                     writable.on_commit_action});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "table_metadata_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaConstraintMutationBatch(
    const EngineRequestContext& context,
    const MgaConstraintMutationBatch& batch) {
  constexpr const char* kOperation = "mga.constraint_mutation_batch";
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation, "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context, kOperation);
  if (authority.error) return authority;
  if (batch.format_version != "neutral_fk_mutation_batch_v1" ||
      // The caller supplies the complete semantics, never the seal.  The
      // engine reserves the MGA event and hashes the final record below.
      !ValidConstraintBatchUuid(
          batch.batch_uuid,
          scratchbird::core::platform::UuidKind::row) ||
      !batch.batch_hash.empty() || batch.mutation_count != 1 ||
      !ValidConstraintBatchUuid(
          batch.database_uuid,
          scratchbird::core::platform::UuidKind::database) ||
      !ValidConstraintBatchUuid(
          batch.constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.database_uuid != context.database_uuid.canonical ||
      !ValidConstraintBatchUuid(
          batch.owner_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.child_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.child_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.child_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.child_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_table_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_schema_uuid,
          scratchbird::core::platform::UuidKind::schema) ||
      !ValidConstraintBatchUuid(
          batch.parent_relation_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.parent_relation_descriptor_generation == 0 ||
      !ValidConstraintBatchUuid(
          batch.parent_column_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.parent_candidate_key_constraint_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.key_descriptor_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      !ValidConstraintBatchUuid(
          batch.support_uuid,
          scratchbird::core::platform::UuidKind::object) ||
      batch.support_family != "btree" ||
      batch.support_policy != "required_exact_unique_index" ||
      batch.match_policy != "simple" ||
      batch.on_update_action != "no_action" ||
      batch.on_delete_action != "no_action" ||
      batch.enforcement_timing != "immediate" ||
      batch.constraint_metadata_generation == 0 ||
      batch.base_table_event_sequence == 0 ||
      batch.parent_base_table_event_sequence == 0 ||
      batch.constraint_kind != "foreign_key" ||
      batch.canonical_constraint_envelope.empty() ||
      batch.updated_table.table_uuid != batch.owner_table_uuid) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "complete_prevalidated_batch_required");
  }
  if (batch.updated_table.temporary ||
      !batch.updated_table.temporary_scope.empty() ||
      !batch.updated_table.temporary_session_uuid.empty() ||
      !batch.updated_table.on_commit_action.empty()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "temporary_constraint_mutation_batch_unsupported");
  }
  const auto current = LoadMgaRelationStoreState(context);
  if (!current.ok) return current.diagnostic;
  const CrudState current_state = BuildCrudCompatibilityStateFromMga(
      current.state);
  const auto current_owner = FindVisibleCrudTable(
      current_state, batch.owner_table_uuid, context.local_transaction_id);
  if (!current_owner ||
      current_owner->event_sequence != batch.base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "owner_metadata_event_changed_before_append");
  }
  const auto current_parent = FindVisibleCrudTable(
      current_state, batch.parent_table_uuid, context.local_transaction_id);
  if (!current_parent ||
      current_parent->event_sequence !=
          batch.parent_base_table_event_sequence) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_metadata_event_changed_before_append");
  }
  const CrudTableRecord& updated = batch.updated_table;
  if (updated.table_uuid != current_owner->table_uuid ||
      updated.default_name != current_owner->default_name ||
      updated.temporary != current_owner->temporary ||
      updated.temporary_scope != current_owner->temporary_scope ||
      updated.temporary_session_uuid !=
          current_owner->temporary_session_uuid ||
      updated.on_commit_action != current_owner->on_commit_action ||
      updated.columns.size() != current_owner->columns.size()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_table_projection_changed");
  }
  std::size_t changed_column_count = 0;
  const std::pair<std::string, std::string>* old_changed_column = nullptr;
  const std::pair<std::string, std::string>* new_changed_column = nullptr;
  for (std::size_t index = 0; index < updated.columns.size(); ++index) {
    const auto& old_column = current_owner->columns[index];
    const auto& new_column = updated.columns[index];
    if (old_column.first != new_column.first) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_column_order_or_name_changed");
    }
    if (old_column.second == new_column.second) continue;
    ++changed_column_count;
    old_changed_column = &old_column;
    new_changed_column = &new_column;
  }
  if (changed_column_count != batch.mutation_count ||
      changed_column_count != 1 || old_changed_column == nullptr ||
      new_changed_column == nullptr) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_mutation_count_mismatch");
  }
  const auto old_fields =
      StrictRelationDescriptorFields(old_changed_column->second);
  const auto new_fields =
      StrictRelationDescriptorFields(new_changed_column->second);
  const auto envelope_fields = StrictRelationDescriptorFields(
      batch.canonical_constraint_envelope);
  if (!old_fields || !new_fields || !envelope_fields) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_descriptor_encoding_invalid");
  }
  const std::set<std::string> fk_projection_fields = {
      "foreign_key",
      "constraint_uuid",
      "constraint_name",
      "constraint_class",
      "owner_object_uuid",
      "owner_object_name",
      "child_column_uuid",
      "referenced_table_uuid",
      "referenced_table_name",
      "referenced_column_uuid",
      "referenced_column",
      "key_descriptor_uuid",
      "referenced_key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "referenced_support_uuid",
      "support_family",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  for (const auto& [key, value] : *old_fields) {
    const auto found = new_fields->find(key);
    if (fk_projection_fields.find(key) != fk_projection_fields.end() ||
        found == new_fields->end() || found->second != value) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_changed_base_column_descriptor");
    }
  }
  for (const auto& [key, value] : *new_fields) {
    (void)value;
    if (old_fields->find(key) == old_fields->end() &&
        fk_projection_fields.find(key) == fk_projection_fields.end()) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "constraint_batch_added_unrelated_column_field");
    }
  }
  auto require_field = [&](const std::map<std::string, std::string>& fields,
                           const char* key,
                           const std::string& expected) {
    const auto found = fields.find(key);
    return found != fields.end() && found->second == expected;
  };
  if (!require_field(*new_fields, "foreign_key", "true") ||
      !require_field(*new_fields, "constraint_uuid", batch.constraint_uuid) ||
      !require_field(*new_fields, "constraint_name", batch.constraint_name) ||
      !require_field(*new_fields, "constraint_class", "foreign_key") ||
      !require_field(*new_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*new_fields, "owner_object_name",
                     current_owner->default_name) ||
      !require_field(*new_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*new_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*new_fields, "referenced_table_name",
                     current_parent->default_name) ||
      !require_field(*new_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*new_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields, "referenced_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*new_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*new_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*new_fields, "referenced_support_uuid",
                     batch.support_uuid) ||
      !require_field(*new_fields, "support_family",
                     batch.support_family) ||
      !require_field(*new_fields, "on_update", batch.on_update_action) ||
      !require_field(*new_fields, "on_delete", batch.on_delete_action) ||
      !require_field(*new_fields, "referential_action", "no_action") ||
      !require_field(*new_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*new_fields, "deferrable", "false") ||
      !require_field(*new_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*new_fields, "constraint_mutation_batch_state",
                     "sealed")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_projection_incoherent");
  }
  const std::set<std::string> final_envelope_fields = {
      "descriptor_version",
      "child_table_uuid",
      "child_column_uuid",
      "child_relation_descriptor_uuid",
      "child_relation_descriptor_generation",
      "parent_table_uuid",
      "parent_column_uuid",
      "parent_relation_descriptor_uuid",
      "parent_relation_descriptor_generation",
      "referenced_table_uuid",
      "referenced_column_uuid",
      "referenced_column",
      "child_column",
      "constraint_name_quoted",
      "on_update",
      "on_delete",
      "referential_action",
      "enforcement_timing",
      "deferrable",
      "constraint_uuid",
      "constraint_name",
      "owner_object_uuid",
      "key_descriptor_uuid",
      "referenced_candidate_key_constraint_uuid",
      "support_uuid",
      "support_family",
      "constraint_mutation_batch_uuid",
      "constraint_mutation_batch_state"};
  std::set<std::string> actual_envelope_fields;
  for (const auto& [key, value] : *envelope_fields) {
    (void)value;
    actual_envelope_fields.insert(key);
  }
  if (actual_envelope_fields != final_envelope_fields ||
      !require_field(*envelope_fields, "descriptor_version",
                     "neutral_fk_single_column_v1") ||
      !require_field(*envelope_fields, "child_table_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "child_column_uuid",
                     batch.child_column_uuid) ||
      !require_field(*envelope_fields, "child_relation_descriptor_uuid",
                     batch.child_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "child_relation_descriptor_generation",
                     std::to_string(
                         batch.child_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "parent_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "parent_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "parent_relation_descriptor_uuid",
                     batch.parent_relation_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "parent_relation_descriptor_generation",
                     std::to_string(
                         batch.parent_relation_descriptor_generation)) ||
      !require_field(*envelope_fields, "referenced_table_uuid",
                     batch.parent_table_uuid) ||
      !require_field(*envelope_fields, "referenced_column_uuid",
                     batch.parent_column_uuid) ||
      !require_field(*envelope_fields, "constraint_uuid",
                     batch.constraint_uuid) ||
      !require_field(*envelope_fields, "constraint_name",
                     batch.constraint_name) ||
      !require_field(*envelope_fields, "owner_object_uuid",
                     batch.owner_table_uuid) ||
      !require_field(*envelope_fields, "key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*envelope_fields,
                     "referenced_candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*envelope_fields, "support_uuid", batch.support_uuid) ||
      !require_field(*envelope_fields, "support_family",
                     batch.support_family) ||
      !require_field(*envelope_fields, "on_update",
                     batch.on_update_action) ||
      !require_field(*envelope_fields, "on_delete",
                     batch.on_delete_action) ||
      !require_field(*envelope_fields, "referential_action", "no_action") ||
      !require_field(*envelope_fields, "enforcement_timing",
                     batch.enforcement_timing) ||
      !require_field(*envelope_fields, "deferrable", "false") ||
      !require_field(*envelope_fields, "constraint_mutation_batch_uuid",
                     batch.batch_uuid) ||
      !require_field(*envelope_fields, "constraint_mutation_batch_state",
                     "sealed") ||
      !require_field(*new_fields, "referenced_column",
                     RelationDescriptorFieldOrEmpty(
                         *envelope_fields, {"referenced_column"}))) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_canonical_envelope_incoherent");
  }
  const auto child_storage = LoadMgaRelationStorageDescriptor(
      context, batch.owner_table_uuid);
  const auto parent_storage = LoadMgaRelationStorageDescriptor(
      context, batch.parent_table_uuid);
  if (!child_storage.ok) return child_storage.diagnostic;
  if (!parent_storage.ok) return parent_storage.diagnostic;
  const auto& child_relation = child_storage.descriptor;
  const auto& parent_relation = parent_storage.descriptor;
  if (child_relation.database_uuid.canonical != batch.database_uuid ||
      child_relation.relation_uuid.canonical != batch.owner_table_uuid ||
      child_relation.schema_uuid.canonical != batch.child_schema_uuid ||
      child_relation.descriptor_uuid.canonical !=
          batch.child_relation_descriptor_uuid ||
      child_relation.descriptor_generation !=
          batch.child_relation_descriptor_generation ||
      parent_relation.database_uuid.canonical != batch.database_uuid ||
      parent_relation.relation_uuid.canonical != batch.parent_table_uuid ||
      parent_relation.schema_uuid.canonical != batch.parent_schema_uuid ||
      parent_relation.descriptor_uuid.canonical !=
          batch.parent_relation_descriptor_uuid ||
      parent_relation.descriptor_generation !=
          batch.parent_relation_descriptor_generation) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_relation_descriptor_binding_changed");
  }
  const auto child_column = std::find_if(
      child_relation.columns.begin(), child_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.child_column_uuid;
      });
  const auto parent_column = std::find_if(
      parent_relation.columns.begin(), parent_relation.columns.end(),
      [&](const MgaRelationColumnStorageDescriptor& column) {
        return column.column_uuid.canonical == batch.parent_column_uuid;
      });
  const std::string quoted = RelationDescriptorFieldOrEmpty(
      *envelope_fields, {"constraint_name_quoted"});
  if (child_column == child_relation.columns.end() ||
      parent_column == parent_relation.columns.end() ||
      child_column->canonical_name_key != new_changed_column->first ||
      !require_field(*envelope_fields, "child_column",
                     child_column->canonical_name_key) ||
      !require_field(*envelope_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      !require_field(*new_fields, "referenced_column",
                     parent_column->canonical_name_key) ||
      (quoted != "true" && quoted != "false")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "constraint_batch_column_descriptor_binding_changed");
  }
  const auto parent_metadata_column = std::find_if(
      current_parent->columns.begin(), current_parent->columns.end(),
      [&](const auto& column) {
        return column.first == parent_column->canonical_name_key;
      });
  if (parent_metadata_column == current_parent->columns.end()) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_column_not_visible");
  }
  const auto parent_key_fields =
      StrictRelationDescriptorFields(parent_metadata_column->second);
  if (!parent_key_fields ||
      !require_field(*parent_key_fields,
                     "candidate_key_constraint_uuid",
                     batch.parent_candidate_key_constraint_uuid) ||
      !require_field(*parent_key_fields,
                     "candidate_key_descriptor_uuid",
                     batch.key_descriptor_uuid) ||
      !require_field(*parent_key_fields, "support_uuid",
                     batch.support_uuid) ||
      !require_field(*parent_key_fields, "support_family", "btree") ||
      (RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "primary_key" &&
       RelationDescriptorFieldOrEmpty(
           *parent_key_fields, {"candidate_key_class"}) != "unique")) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_projection_changed");
  }
  const auto descriptor_support = std::find_if(
      parent_relation.indexes.begin(), parent_relation.indexes.end(),
      [&](const MgaRelationIndexStorageDescriptor& index) {
        return index.index_uuid.canonical == batch.support_uuid &&
               index.unique && index.family == "btree";
      });
  auto key_columns = [](const CrudIndexRecord& index) {
    std::vector<std::string> columns;
    for (const std::string& envelope : index.key_envelopes) {
      if (envelope.empty() || envelope == "unique" ||
          envelope == "primary_key" || envelope.starts_with("include:") ||
          envelope.starts_with("where_eq:") ||
          envelope.starts_with("where_mod_eq:") ||
          envelope == "where_true") {
        continue;
      }
      if (envelope.starts_with("identity:")) {
        columns.push_back(envelope.substr(9));
      } else if (envelope.starts_with("desc:")) {
        columns.push_back(envelope.substr(5));
      } else if (envelope.starts_with("cast:")) {
        const std::string rest = envelope.substr(5);
        const auto separator = rest.find(':');
        columns.push_back(separator == std::string::npos
                              ? rest
                              : rest.substr(0, separator));
      } else {
        columns.push_back(envelope);
      }
    }
    if (columns.empty() && !index.column_name.empty()) {
      columns.push_back(index.column_name);
    }
    return columns;
  };
  std::size_t exact_support_count = 0;
  for (const auto& index : VisibleCrudIndexesForTable(
           current_state,
           current_parent->table_uuid,
           context.local_transaction_id)) {
    const auto columns = key_columns(index);
    const std::string visible_support_family =
        index.family.empty() ? CrudIndexFamilyForProfile(index.profile)
                             : index.family;
    if (index.index_uuid == batch.support_uuid && index.unique &&
        visible_support_family == batch.support_family &&
        columns.size() == 1 &&
        columns.front() == parent_column->canonical_name_key) {
      ++exact_support_count;
    }
  }
  if (descriptor_support == parent_relation.indexes.end() ||
      exact_support_count != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "parent_candidate_key_exact_support_changed");
  }
  // D1 admits one immediate single-column FK per child table.  Enforce that
  // bounded generation model in the storage authority as well as the DDL
  // adapter so generation 1 can never silently duplicate.
  if (batch.constraint_metadata_generation != 1) {
    return MakeInvalidRequestDiagnostic(
        kOperation, "bounded_d1_constraint_generation_must_be_one");
  }
  for (const auto& [column_name, descriptor] : current_owner->columns) {
    (void)column_name;
    if (descriptor.find("constraint_mutation_batch_state=sealed") !=
        std::string::npos) {
      return MakeInvalidRequestDiagnostic(
          kOperation, "bounded_d1_prior_constraint_batch_unsupported");
    }
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) return reservation.diagnostic;

  CrudTableRecord table = batch.updated_table;
  table.creator_tx = context.local_transaction_id;
  table.event_sequence = reservation.first;
  MgaConstraintMutationBatch sealed_batch = batch;
  sealed_batch.updated_table = table;
  sealed_batch.batch_hash = ComputeMgaConstraintMutationBatchHash(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (sealed_batch.batch_hash.empty()) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_hash_generation_failed");
  }
  // `sealed` is emitted here, after all required catalog and relation fields
  // have passed validation, and is never copied from an SBLR operand.
  const auto line_fields = ConstraintMutationBatchLineFields(
      sealed_batch, table.creator_tx, table.event_sequence);
  if (line_fields.size() != constraint_batch_field::kFieldCount) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "batch_codec_field_count_invalid");
  }
  const std::string line = JoinLine(line_fields);
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic(kOperation,
                                        "sealed_batch_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaIndexMetadata(const EngineRequestContext& context,
                                           const CrudIndexRecord& index) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "database_path_required");
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(context, "mga.relation_metadata.index_create");
  if (authority.error) { return authority; }
  CrudIndexRecord writable = index;
  writable.creator_tx = context.local_transaction_id;
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  writable.event_sequence = reservation.first;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "INDEX_METADATA",
                                     std::to_string(writable.creator_tx),
                                     std::to_string(writable.event_sequence),
                                     writable.index_uuid,
                                     writable.table_uuid,
                                     NormalizeCrudIndexProfile(writable.profile),
                                     writable.family.empty() ? CrudIndexFamilyForProfile(writable.profile) : writable.family,
                                     EncodeCrudText(writable.default_name),
                                     EncodeCrudText(writable.column_name),
                                     EncodeStringListAsCrudPairs(writable.key_envelopes),
                                     EncodeStringListAsCrudPairs(writable.include_columns),
                                     writable.predicate_kind,
                                     EncodeCrudText(writable.predicate_column),
                                     EncodeCrudText(writable.predicate_value),
                                     writable.unique ? "1" : "0",
                                     writable.exact_fallback ? "1" : "0"});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.relation_metadata", "index_metadata_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaIndexEntriesForRow(const EngineRequestContext& context,
                                                const CrudState& state,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                const std::vector<std::pair<std::string, std::string>>& values) {
  return AppendMgaIndexEntriesForRows(context,
                                      state,
                                      table_uuid,
                                      std::vector<MgaIndexEntryRowInput>{{row_uuid, version_uuid, values}});
}

EngineApiDiagnostic AppendMgaIndexEntriesForRows(const EngineRequestContext& context,
                                                 const CrudState& state,
                                                 const std::string& table_uuid,
                                                 const std::vector<MgaIndexEntryRowInput>& rows) {
  if (rows.empty()) {
    return OkDiagnostic();
  }
  const auto indexes = VisibleCrudIndexesForTable(state, table_uuid, context.local_transaction_id);
  if (indexes.empty()) {
    return OkDiagnostic();
  }
  return AppendMgaIndexEntriesForRowsWithIndexes(context, indexes, table_uuid, rows);
}

EngineApiDiagnostic AppendMgaIndexEntriesForRowsWithIndexes(const EngineRequestContext& context,
                                                            const std::vector<CrudIndexRecord>& indexes,
                                                            const std::string& table_uuid,
                                                            const std::vector<MgaIndexEntryRowInput>& rows) {
  if (rows.empty() || indexes.empty()) {
    return OkDiagnostic();
  }
  std::vector<MgaIndexEntryAppendBatch> batches;
  batches.reserve(indexes.size());
  for (const auto& index : indexes) {
    MgaIndexEntryAppendBatch batch;
    batch.index = index;
    batch.table_uuid = table_uuid;
    batch.rows = rows;
    batches.push_back(std::move(batch));
  }
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendIndexEntryBatches(batches);
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

EngineApiDiagnostic AppendMgaExactIndexEntryBatches(
    const EngineRequestContext& context,
    const std::vector<MgaExactIndexEntryAppendBatch>& batches) {
  bool empty = true;
  for (const auto& batch : batches) {
    if (!batch.entries.empty()) {
      empty = false;
      break;
    }
  }
  if (empty) {
    return OkDiagnostic();
  }
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendExactIndexEntryBatches(batches);
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

MgaSecondaryIndexDeltaLedgerResult LoadMgaSecondaryIndexDeltaLedger(
    const EngineRequestContext& context) {
  return LoadSecondaryIndexDeltaLedgerFromPath(context);
}

EngineApiDiagnostic AppendMgaSecondaryIndexDeltaLedgerEntries(
    const EngineRequestContext& context,
    const std::vector<MgaSecondaryIndexDeltaLedgerEntryInput>& entries,
    std::vector<EngineEvidenceReference>* evidence) {
  // DPC_DEFERRED_INDEX_WRITE_PATH
  if (entries.empty()) {
    return OkDiagnostic();
  }
  const auto authority = ValidateMgaMutatingTransactionAuthority(
      context,
      "mga.secondary_index_delta_ledger.append");
  if (authority.error) { return authority; }
  if (context.transaction_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                        "transaction_uuid_required");
  }

  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  auto ledger = loaded.ledger;
  const std::uint64_t existing_delta_record_count =
      static_cast<std::uint64_t>(ledger.records.size());
  std::vector<idx::SecondaryIndexDeltaLedgerRecord> staged_records;
  for (const auto& input : entries) {
    if (IsUniqueMgaIndex(input.index)) {
      return MakeInvalidRequestDiagnostic("mga.secondary_index_delta_ledger",
                                          "unique_index_delta_refused");
    }
    const auto keys = CrudIndexKeysForValues(input.index, input.values);
    for (const auto& key : keys) {
      idx::SecondaryIndexDeltaLedgerRecord record;
      const auto built = BuildSecondaryIndexDeltaLedgerRecord(context, input, key, &record);
      if (built.error) { return built; }
      const auto appended = idx::AppendPersistentSecondaryIndexDelta(
          &ledger,
          record,
          DefaultSecondaryIndexDeltaLedgerLimits());
      if (!appended.ok()) {
        return DiagnosticFromSecondaryIndexDeltaLedger(
            appended.diagnostic,
            "SB-MGA-SECONDARY-DELTA-APPEND-FAILED",
            "mga.secondary_index_delta_ledger.append_failed");
      }
      staged_records.push_back(std::move(record));
    }
  }
  if (staged_records.empty()) {
    return OkDiagnostic();
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "secondary_index_delta_ledger",
      SecondaryIndexDeltaLedgerStorePath(context),
      static_cast<std::uint64_t>(staged_records.size()),
      [existing_delta_record_count]() { return existing_delta_record_count + 1; });
  if (!reservation.ok) { return reservation.diagnostic; }
  const auto written = WriteSecondaryIndexDeltaLedger(context, ledger);
  if (written.error) { return written; }
  if (evidence != nullptr) {
    AddEventSequenceReservationEvidence(evidence, reservation);
    for (const auto& record : staged_records) {
      evidence->push_back({"mga_secondary_index_delta_ledger",
                           scratchbird::core::uuid::UuidToString(record.delta.delta_id.value)});
      evidence->push_back({"mga_secondary_index_delta_index",
                           scratchbird::core::uuid::UuidToString(record.delta.index_uuid.value)});
      evidence->push_back({"mga_secondary_index_delta_kind",
                           idx::SecondaryIndexDeltaKindName(record.delta.delta_kind)});
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic CommitMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id) {
  if (local_transaction_id == 0) {
    return OkDiagnostic();
  }
  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  bool changed = false;
  for (auto& record : loaded.ledger.records) {
    if (record.delta.local_transaction_id != local_transaction_id) {
      continue;
    }
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted) {
      record.commit_state =
          idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
      record.delta.committed = true;
      changed = true;
    }
  }
  if (!changed) {
    return OkDiagnostic();
  }
  return WriteSecondaryIndexDeltaLedger(context, loaded.ledger);
}

EngineApiDiagnostic RollbackMgaSecondaryIndexDeltaLedgerTransaction(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id) {
  if (local_transaction_id == 0) {
    return OkDiagnostic();
  }
  auto loaded = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  const auto before = loaded.ledger.records.size();
  loaded.ledger.records.erase(
      std::remove_if(loaded.ledger.records.begin(),
                     loaded.ledger.records.end(),
                     [local_transaction_id](const idx::SecondaryIndexDeltaLedgerRecord& record) {
                       return record.delta.local_transaction_id == local_transaction_id;
                     }),
      loaded.ledger.records.end());
  if (loaded.ledger.records.size() == before) {
    return OkDiagnostic();
  }
  return WriteSecondaryIndexDeltaLedger(context, loaded.ledger);
}

MgaSecondaryIndexDeltaMergeAgentResult MergeMgaSecondaryIndexDeltasForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaMergeAgentRequest& request) {
  // DPC_SECONDARY_INDEX_DELTA_MERGE_AGENT_GATE: supplied-horizon merge path.
  MgaSecondaryIndexDeltaMergeAgentResult result;
  result.index_uuid = request.index_uuid;
  result.table_uuid = request.table_uuid;
  result.authoritative_cleanup_horizon_local_transaction_id =
      request.authoritative_cleanup_horizon_local_transaction_id;
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_index", request.index_uuid);
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_table", request.table_uuid);
  AddMergeEvidence(&result.evidence,
                   "mga_secondary_index_delta_merge_horizon",
                   std::to_string(request.authoritative_cleanup_horizon_local_transaction_id));

  if (request.merge_disabled) {
    result.throttle_or_refusal_reason = "merge_agent_disabled";
    result.diagnostic = Dpc024MergeDiagnostic("merge_agent_disabled",
                                              "mga.secondary_index_delta_merge.disabled",
                                              "secondary-index delta merge agent is disabled by request");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (!request.cleanup_horizon_authoritative) {
    result.throttle_or_refusal_reason = "not_authoritative_horizon";
    result.diagnostic = Dpc024MergeDiagnostic("not_authoritative_horizon",
                                              "mga.secondary_index_delta_merge.not_authoritative_horizon",
                                              "authoritative cleanup horizon is required");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_merge == 0) {
    result.throttled = true;
    result.throttle_or_refusal_reason = "resource_governor_throttled";
    result.diagnostic = Dpc024MergeDiagnostic("resource_governor_throttled",
                                              "mga.secondary_index_delta_merge.resource_governor_throttled",
                                              "nonzero scan and merge budgets are required");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.state_load_refused",
                                              loaded_state.diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.index_not_found",
                                              "requested index/table identity is not visible");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (IsUniqueMgaIndex(*selected)) {
    result.throttle_or_refusal_reason = "unique_index_delta_refused";
    result.diagnostic = Dpc024MergeDiagnostic("unique_index_delta_refused",
                                              "mga.secondary_index_delta_merge.unique_index_delta_refused",
                                              "unique secondary indexes cannot use deferred delta merge");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.ledger_load_refused",
                                              loaded_ledger.diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (loaded_ledger.ledger.records.size() > request.max_records_to_scan) {
    result.throttled = true;
    result.throttle_or_refusal_reason = "resource_governor_throttled";
    result.scanned_count = request.max_records_to_scan;
    result.retained_count = loaded_ledger.ledger.records.size();
    result.diagnostic = Dpc024MergeDiagnostic("resource_governor_throttled",
                                              "mga.secondary_index_delta_merge.resource_governor_throttled",
                                              "persistent delta ledger exceeds bounded scan budget");
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_retained_count",
                     std::to_string(result.retained_count));
    return result;
  }

  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.recovery_refused",
                                              recovery.stable_reason);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  if (request.ipar_fault_injection_point == "secondary_merge") {
    result.throttle_or_refusal_reason = "ipar_fault_injection_secondary_merge";
    result.diagnostic = IparFaultDiagnostic("mga.secondary_index_delta_merge",
                                            "secondary_merge",
                                            "phase=before_merge_publish");
    AddMergeEvidence(&result.evidence,
                     "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    AddMergeEvidence(&result.evidence,
                     "mga_secondary_index_delta_merge_authority",
                     kIparP706Authority);
    AppendIparFaultEvidence(&result.evidence,
                            "secondary_merge",
                            "retain_delta_overlay_or_retry_under_inventory_horizon");
    return result;
  }

  idx::SecondaryIndexDeltaLedger staged_delta_ledger;
  std::vector<std::size_t> processed_record_indexes;
  std::uint64_t eligible_count = 0;
  for (std::size_t i = 0; i < loaded_ledger.ledger.records.size(); ++i) {
    const auto& record = loaded_ledger.ledger.records[i];
    if (LedgerRecordBelongsToUniqueIndex(record, state)) {
      result.throttle_or_refusal_reason = "unique_index_delta_refused";
      result.diagnostic = Dpc024MergeDiagnostic("unique_index_delta_refused",
                                                "mga.secondary_index_delta_merge.unique_index_delta_refused",
                                                "persistent ledger contains a unique-index delta record");
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    if (!LedgerRecordRelevantToIndex(record, *selected, request.table_uuid)) {
      continue;
    }
    ++result.scanned_count;
    if (record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required ||
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::refused) {
      result.throttle_or_refusal_reason = "corrupt_ledger_refused";
      result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                                "mga.secondary_index_delta_merge.record_state_refused",
                                                "persistent ledger record is repair-required or refused");
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    const bool eligible =
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge &&
        record.delta.committed &&
        record.delta.local_transaction_id <= request.authoritative_cleanup_horizon_local_transaction_id;
    if (!eligible) {
      ++result.retained_count;
      continue;
    }
    ++eligible_count;
    if (processed_record_indexes.size() >= request.max_records_to_merge) {
      ++result.retained_count;
      result.throttled = true;
      result.throttle_or_refusal_reason = "resource_governor_throttled";
      continue;
    }
    staged_delta_ledger.deltas.push_back(record.delta);
    processed_record_indexes.push_back(i);
  }

  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  base_entries.reserve(state.index_entries.size() + staged_delta_ledger.deltas.size());
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != request.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto converted = BaseEntryForOverlay(entry, &base);
    if (converted.error) {
      result.throttle_or_refusal_reason = "corrupt_ledger_refused";
      result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                                "mga.secondary_index_delta_merge.base_entry_refused",
                                                converted.detail);
      AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                       result.throttle_or_refusal_reason);
      return result;
    }
    base.committed_local_transaction_id = entry.creator_tx;
    base_entries.push_back(std::move(base));
  }

  idx::SecondaryIndexDeltaMergeLedger merge_ledger;
  idx::SecondaryIndexMergeRequest core_request;
  auto diagnostic = ParseLedgerTypedUuid(request.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &core_request.index_uuid);
  if (diagnostic.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.index_uuid_refused",
                                              diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  diagnostic = ParseLedgerTypedUuid(request.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &core_request.table_uuid);
  if (diagnostic.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.table_uuid_refused",
                                              diagnostic.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }
  core_request.authoritative_cleanup_horizon_local_transaction_id =
      request.authoritative_cleanup_horizon_local_transaction_id;
  core_request.cleanup_horizon_authoritative = request.cleanup_horizon_authoritative;
  core_request.index_kind = idx::SecondaryIndexKind::non_unique;
  core_request.max_records_to_scan = request.max_records_to_scan;
  core_request.max_records_to_merge = request.max_records_to_merge;
  core_request.merge_disabled = request.merge_disabled;
  const auto merged = idx::MergeSecondaryIndexDeltas(
      &merge_ledger,
      &base_entries,
      &staged_delta_ledger,
      core_request);
  if (!merged.ok()) {
    result.throttled = merged.throttled;
    result.throttle_or_refusal_reason = merged.diagnostic.diagnostic_code;
    result.diagnostic = Dpc024MergeDiagnostic(merged.diagnostic.diagnostic_code,
                                              merged.diagnostic.message_key,
                                              merged.diagnostic.remediation_hint);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  const auto rewritten = RewriteMgaIndexEntriesForMergedIndex(
      context,
      state,
      *selected,
      request.table_uuid,
      base_entries);
  if (rewritten.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = rewritten;
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  for (std::size_t index : processed_record_indexes) {
    loaded_ledger.ledger.records[index].commit_state =
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned;
  }
  const auto written = WriteSecondaryIndexDeltaLedger(context, loaded_ledger.ledger);
  if (written.error) {
    result.throttle_or_refusal_reason = "corrupt_ledger_refused";
    result.diagnostic = Dpc024MergeDiagnostic("corrupt_ledger_refused",
                                              "mga.secondary_index_delta_merge.ledger_replace_refused",
                                              written.detail);
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_refused",
                     result.throttle_or_refusal_reason);
    return result;
  }

  result.ok = true;
  result.merged_count = merged.evidence.merged_count;
  result.cleaned_count = static_cast<std::uint64_t>(processed_record_indexes.size());
  result.diagnostic = OkDiagnostic();
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_result",
                   result.throttled ? "resource_governor_throttled" : "successful_merge");
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_merged_count",
                   std::to_string(result.merged_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_retained_count",
                   std::to_string(result.retained_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_cleaned_count",
                   std::to_string(result.cleaned_count));
  AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_scanned_count",
                   std::to_string(result.scanned_count));
  if (result.throttled) {
    AddMergeEvidence(&result.evidence, "mga_secondary_index_delta_merge_throttle_reason",
                     result.throttle_or_refusal_reason);
  }
  (void)eligible_count;
  return result;
}

MgaSecondaryIndexDeltaRecoveryRepairResult ValidateAndRepairMgaSecondaryIndexDeltaLedgerForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexDeltaRecoveryRepairRequest& request) {
  // DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR: engine-owned bounded recovery
  // validation. Transaction finality comes only from durable MGA inventory.
  MgaSecondaryIndexDeltaRecoveryRepairResult result;
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_search_key",
                      "DPC_SECONDARY_INDEX_DELTA_RECOVERY_REPAIR");
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_index",
                      request.index_uuid);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_table",
                      request.table_uuid);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_scan_budget",
                      std::to_string(request.max_records_to_scan));

  auto refuse = [&](std::string code,
                    std::string message_key,
                    std::string detail) {
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    result.diagnostic = Dpc025RecoveryDiagnostic(code, message_key, detail);
    AddRecoveryEvidence(&result.evidence,
                        "mga_secondary_index_delta_recovery_refused",
                        result.diagnostic.code);
    return result;
  };

  if (request.index_uuid.empty() || request.table_uuid.empty()) {
    return refuse("secondary_index_delta_recovery_invalid_request",
                  "mga.secondary_index_delta_recovery.invalid_request",
                  "index_uuid and table_uuid are required");
  }
  if (request.max_records_to_scan == 0) {
    return refuse("secondary_index_delta_recovery_scan_budget_exhausted",
                  "mga.secondary_index_delta_recovery.scan_budget_exhausted",
                  "recovery validation requires a nonzero bounded scan budget");
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    return refuse("secondary_index_delta_recovery_state_load_refused",
                  "mga.secondary_index_delta_recovery.state_load_refused",
                  loaded_state.diagnostic.detail);
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    return refuse("secondary_index_delta_recovery_index_not_found",
                  "mga.secondary_index_delta_recovery.index_not_found",
                  "requested index/table identity is not visible to recovery validation");
  }
  if (IsUniqueMgaIndex(*selected)) {
    return refuse("unique_index_delta_refused",
                  "mga.secondary_index_delta_recovery.unique_index_delta_refused",
                  "unique secondary indexes cannot use deferred delta recovery repair");
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    result.diagnostic = loaded_ledger.diagnostic;
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    AddRecoveryEvidence(&result.evidence,
                        "mga_secondary_index_delta_recovery_refused",
                        result.diagnostic.code);
    return result;
  }

  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  result.recovery_class =
      idx::SecondaryIndexDeltaLedgerRecoveryClassName(recovery.recovery_class);
  result.recovery_action =
      idx::SecondaryIndexDeltaLedgerRecoveryActionName(recovery.action);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_class",
                      result.recovery_class);
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_action",
                      result.recovery_action);
  if (recovery.fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open) {
    return refuse("secondary_index_delta_recovery_ledger_refused",
                  "mga.secondary_index_delta_recovery.ledger_refused",
                  recovery.stable_reason);
  }

  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    return refuse(loaded_inventory.diagnostic.diagnostic_code.empty()
                      ? "secondary_index_delta_recovery_inventory_load_refused"
                      : loaded_inventory.diagnostic.diagnostic_code,
                  loaded_inventory.diagnostic.message_key.empty()
                      ? "mga.secondary_index_delta_recovery.inventory_load_refused"
                      : loaded_inventory.diagnostic.message_key,
                  loaded_inventory.diagnostic.remediation_hint);
  }

  idx::PersistentSecondaryIndexDeltaLedger repaired_ledger = loaded_ledger.ledger;
  std::vector<idx::SecondaryIndexDeltaLedgerRecord> retained_records;
  retained_records.reserve(repaired_ledger.records.size());
  bool changed = false;
  for (const auto& record : repaired_ledger.records) {
    if (!LedgerRecordRelevantToIndex(record, *selected, request.table_uuid)) {
      retained_records.push_back(record);
      continue;
    }
    ++result.scanned_count;
    if (result.scanned_count > request.max_records_to_scan) {
      return refuse("secondary_index_delta_recovery_scan_budget_exhausted",
                    "mga.secondary_index_delta_recovery.scan_budget_exhausted",
                    "persistent delta ledger exceeds bounded recovery scan budget");
    }
    if (LedgerRecordBelongsToUniqueIndex(record, state)) {
      return refuse("unique_index_delta_refused",
                    "mga.secondary_index_delta_recovery.unique_index_delta_refused",
                    "persistent ledger contains a unique-index delta record");
    }

    idx::SecondaryIndexDeltaLedgerRecord staged = record;
    bool keep_record = true;
    switch (record.commit_state) {
      case idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted: {
        const auto lookup = LookupLocalTransaction(
            loaded_inventory.inventory,
            MakeLocalTransactionId(record.delta.local_transaction_id));
        if (!lookup.ok()) {
          return refuse("secondary_index_delta_recovery_missing_transaction_authority",
                        "mga.secondary_index_delta_recovery.missing_transaction_authority",
                        "precommit delta has no durable MGA transaction inventory entry");
        }
        if (IsDpc025CommittedTerminal(lookup.entry.state)) {
          staged.commit_state =
              idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge;
          staged.delta.committed = true;
          ++result.promoted_count;
          changed = true;
        } else if (IsDpc025RolledBackTerminal(lookup.entry.state)) {
          keep_record = false;
          ++result.removed_count;
          changed = true;
        } else {
          ++result.retained_count;
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge: {
        const auto lookup = LookupLocalTransaction(
            loaded_inventory.inventory,
            MakeLocalTransactionId(record.delta.local_transaction_id));
        if (!lookup.ok()) {
          return refuse("secondary_index_delta_recovery_missing_transaction_authority",
                        "mga.secondary_index_delta_recovery.missing_transaction_authority",
                        "committed_premerge delta has no durable MGA transaction inventory entry");
        }
        if (IsDpc025CommittedTerminal(lookup.entry.state)) {
          ++result.committed_premerge_count;
          ++result.retained_count;
        } else if (IsDpc025RolledBackTerminal(lookup.entry.state)) {
          keep_record = false;
          ++result.removed_count;
          changed = true;
        } else {
          return refuse("secondary_index_delta_recovery_commit_state_inconsistent",
                        "mga.secondary_index_delta_recovery.commit_state_inconsistent",
                        "committed_premerge delta is not backed by committed MGA inventory state");
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned: {
        ++result.merged_cleaned_count;
        ++result.retained_count;
        if (request.require_authoritative_base &&
            Dpc025DeltaRequiresPublishedBase(record.delta.delta_kind)) {
          EngineApiDiagnostic base_diagnostic;
          if (!Dpc025PublishedBaseContainsRecord(state,
                                                 *selected,
                                                 request.table_uuid,
                                                 record,
                                                 &base_diagnostic)) {
            result.diagnostic = base_diagnostic;
            result.ok = false;
            result.refused = true;
            result.fail_closed = true;
            AddRecoveryEvidence(&result.evidence,
                                "mga_secondary_index_delta_recovery_refused",
                                result.diagnostic.code);
            return result;
          }
        }
        break;
      }
      case idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required:
      case idx::SecondaryIndexDeltaLedgerCommitState::refused:
        return refuse("secondary_index_delta_recovery_record_state_refused",
                      "mga.secondary_index_delta_recovery.record_state_refused",
                      "persistent ledger contains repair-required or refused record state");
    }

    if (keep_record) {
      retained_records.push_back(std::move(staged));
    }
  }

  if (changed && !request.repair_enabled) {
    return refuse("secondary_index_delta_recovery_repair_disabled",
                  "mga.secondary_index_delta_recovery.repair_disabled",
                  "bounded repair is required but was not enabled by request");
  }
  if (changed) {
    repaired_ledger.records = std::move(retained_records);
    const auto written = WriteSecondaryIndexDeltaLedger(context, repaired_ledger);
    if (written.error) {
      return refuse("secondary_index_delta_recovery_write_refused",
                    "mga.secondary_index_delta_recovery.write_refused",
                    written.detail);
    }
    result.repaired = true;
  }

  result.ok = true;
  result.fail_closed = false;
  result.diagnostic = OkDiagnostic();
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_result",
                      result.repaired ? "repaired" : "validated");
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_scanned_count",
                      std::to_string(result.scanned_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_retained_count",
                      std::to_string(result.retained_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_removed_count",
                      std::to_string(result.removed_count));
  AddRecoveryEvidence(&result.evidence,
                      "mga_secondary_index_delta_recovery_promoted_count",
                      std::to_string(result.promoted_count));
  return result;
}

MgaSecondaryIndexGarbageCleanupResult CleanupMgaSecondaryIndexGarbageForIndex(
    const EngineRequestContext& context,
    const MgaSecondaryIndexGarbageCleanupRequest& request) {
  // DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT: engine-owned cleanup using
  // DPC-030 durable MGA cleanup horizon authority.
  MgaSecondaryIndexGarbageCleanupResult result;
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_search_key",
                                 "DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT");
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_index",
                                 request.index_uuid);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_table",
                                 request.table_uuid);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_scan_budget",
                                 std::to_string(request.max_records_to_scan));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_clean_budget",
                                 std::to_string(request.max_records_to_clean));

  auto refuse = [&](std::string code,
                    std::string message_key,
                    std::string detail) {
    result.ok = false;
    result.refused = true;
    result.fail_closed = true;
    result.decision = "refused";
    result.diagnostic = Dpc033CleanupDiagnostic(code, message_key, detail);
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup_refused",
                                   result.diagnostic.code);
    return result;
  };

  if (request.index_uuid.empty() || request.table_uuid.empty()) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.invalid_identity",
                  "index_uuid and table_uuid are required");
  }
  if (request.max_records_to_scan == 0 || request.max_records_to_clean == 0) {
    return refuse("INDEX_GARBAGE_CLEANUP.BUDGET_REQUIRED",
                  "mga.secondary_index_garbage_cleanup.budget_required",
                  "cleanup requires nonzero bounded scan and clean budgets");
  }

  auto loaded_state = LoadMgaRelationStoreState(context);
  if (!loaded_state.ok) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.state_load_refused",
                  loaded_state.diagnostic.detail);
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded_state.state);
  const auto selected = FindVisibleCrudIndexByUuid(
      state,
      request.table_uuid,
      request.index_uuid,
      context.local_transaction_id);
  if (!selected) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.index_not_found",
                  "requested index/table identity is not visible");
  }
  if (IsUniqueMgaIndex(*selected)) {
    return refuse("INDEX_GARBAGE_CLEANUP.UNIQUE_INDEX_REFUSED",
                  "mga.secondary_index_garbage_cleanup.unique_index_refused",
                  "unique secondary indexes remain on the synchronous path");
  }

  const auto loaded_inventory =
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded_inventory.ok()) {
    return refuse(loaded_inventory.diagnostic.diagnostic_code.empty()
                      ? "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL"
                      : loaded_inventory.diagnostic.diagnostic_code,
                  loaded_inventory.diagnostic.message_key.empty()
                      ? "mga.secondary_index_garbage_cleanup.inventory_load_refused"
                      : loaded_inventory.diagnostic.message_key,
                  loaded_inventory.diagnostic.remediation_hint);
  }

  auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.ledger_load_refused",
                  loaded_ledger.diagnostic.detail);
  }
  const auto recovery = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(
      loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                  "mga.secondary_index_garbage_cleanup.recovery_refused",
                  recovery.stable_reason);
  }

  agents::IndexGarbageCleanupAgentRequest agent_request;
  agent_request.horizon_request.inventory = loaded_inventory.inventory;
  agent_request.horizon_request.inventory_authoritative =
      request.inventory_authoritative;
  agent_request.horizon_request.inventory_complete = request.inventory_complete;
  agent_request.horizon_request.active_snapshot_inventory_authoritative =
      request.active_snapshot_inventory_authoritative;
  agent_request.index_kind = idx::SecondaryIndexKind::non_unique;
  agent_request.ledger = loaded_ledger.ledger;
  agent_request.max_records_to_scan = request.max_records_to_scan;
  agent_request.max_records_to_clean = request.max_records_to_clean;
  agent_request.engine_mga_authoritative = request.engine_mga_authoritative;
  auto diagnostic = ParseLedgerTypedUuid(request.index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &agent_request.index_uuid);
  if (diagnostic.error) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.index_uuid_refused",
                  diagnostic.detail);
  }
  diagnostic = ParseLedgerTypedUuid(request.table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &agent_request.table_uuid);
  if (diagnostic.error) {
    return refuse("INDEX_GARBAGE_CLEANUP.INVALID_IDENTITY",
                  "mga.secondary_index_garbage_cleanup.table_uuid_refused",
                  diagnostic.detail);
  }

  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != request.table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    diagnostic = BaseEntryForOverlay(entry, &base);
    if (diagnostic.error) {
      return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                    "mga.secondary_index_garbage_cleanup.base_entry_refused",
                    diagnostic.detail);
    }
    base.committed_local_transaction_id = entry.creator_tx;
    agent_request.base_entries.push_back(std::move(base));
  }

  for (const auto& row :
       VisibleCrudRowsForContext(state, request.table_uuid, context)) {
    for (const auto& key : CrudIndexKeysForValues(*selected, row.values)) {
      idx::SecondaryIndexTableSnapshotEntry snapshot;
      diagnostic = Dpc033TableSnapshotEntryForCleanup(
          *selected,
          row,
          key,
          &snapshot);
      if (diagnostic.error) {
        return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                      "mga.secondary_index_garbage_cleanup.table_snapshot_refused",
                      diagnostic.detail);
      }
      agent_request.table_snapshot.push_back(std::move(snapshot));
    }
  }

  const auto agent = agents::RunIndexGarbageCleanupAgentBatch(agent_request);
  result.decision = idx::SecondaryIndexGarbageCleanupDecisionKindName(agent.decision);
  result.fail_closed = agent.fail_closed;
  result.refused = agent.fail_closed;
  result.budget_exhausted = agent.budget_exhausted;
  result.horizon_blocked = agent.horizon_blocked;
  result.validation_before_ok = agent.validation_before_ok;
  result.validation_after_ok = agent.validation_after_ok;
  result.before_delta_ledger_records = agent.before.delta_ledger_records;
  result.after_delta_ledger_records = agent.after.delta_ledger_records;
  result.cleaned_count = agent.after.cleaned_garbage_records;
  result.retained_count = agent.after.relevant_delta_records;
  result.scanned_count = agent.before.scanned_delta_records;
  if (agent.horizon.cleanup_horizon.valid()) {
    result.authoritative_cleanup_horizon_local_transaction_id =
        agent.horizon.cleanup_horizon.value;
  }
  for (const auto& field : agent.evidence) {
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup." + field.key,
                                   field.value);
  }
  result.diagnostic = Dpc033CleanupDiagnostic(
      agent.diagnostic.diagnostic_code,
      agent.diagnostic.message_key,
      Dpc033DiagnosticDetail(agent.diagnostic),
      agent.fail_closed);
  if (!agent.ok()) {
    AddIndexGarbageCleanupEvidence(&result.evidence,
                                   "mga_secondary_index_garbage_cleanup_refused",
                                   result.diagnostic.code);
    return result;
  }

  if (result.cleaned_count != 0) {
    const auto written = WriteSecondaryIndexDeltaLedger(context, agent.cleaned_ledger);
    if (written.error) {
      return refuse("INDEX_GARBAGE_CLEANUP.VALIDATION_REFUSED",
                    "mga.secondary_index_garbage_cleanup.ledger_replace_refused",
                    written.detail);
    }
  }

  result.ok = true;
  result.refused = false;
  result.fail_closed = false;
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_result",
                                 result.decision);
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_cleaned_count",
                                 std::to_string(result.cleaned_count));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_retained_count",
                                 std::to_string(result.retained_count));
  AddIndexGarbageCleanupEvidence(&result.evidence,
                                 "mga_secondary_index_garbage_cleanup_scanned_count",
                                 std::to_string(result.scanned_count));
  return result;
}

MgaIndexedRowsLookupResult IndexedMgaRowsForPredicateForContext(
    const CrudState& state,
    const std::string& table_uuid,
    const EnginePredicateEnvelope& predicate,
    const EngineRequestContext& context,
    std::uint64_t limit) {
  // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP: non-unique deferred secondary
  // index readers must combine persisted base entries with visible MGA deltas.
  MgaIndexedRowsLookupResult result;
  const auto selected = SelectCrudIndexForPredicate(
      state,
      table_uuid,
      predicate,
      context.local_transaction_id);
  if (!selected) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "no_usable_index"});
    return result;
  }

  if (IsUniqueMgaIndex(*selected)) {
    result.rows = IndexedCrudRowsForPredicateForContext(
        state,
        table_uuid,
        predicate,
        context,
        limit,
        &result.index_evidence_id);
    result.ok = true;
    result.index_used = !result.index_evidence_id.empty();
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "unique_synchronous_bypass"});
    return result;
  }

  const auto loaded_ledger = LoadSecondaryIndexDeltaLedgerFromPath(context);
  if (!loaded_ledger.ok) {
    return RefuseIndexedLookup(
        "secondary_index_delta_ledger_load_refused",
        loaded_ledger.diagnostic);
  }

  const auto recovery =
      idx::ClassifySecondaryIndexDeltaLedgerForRecovery(loaded_ledger.ledger);
  if (!recovery.ok() ||
      recovery.recovery_class ==
          idx::SecondaryIndexDeltaLedgerRecoveryClass::repair_rebuild_required ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::refuse_open ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::fail_closed ||
      recovery.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base) {
    return RefuseIndexedLookup(
        "secondary_index_delta_ledger_recovery_refused",
        OverlayLookupDiagnostic(recovery.diagnostic,
                                "SB-MGA-SECONDARY-DELTA-RECOVERY-REFUSED",
                                "mga.secondary_index_delta_ledger.recovery_refused"));
  }

  bool has_relevant_delta = false;
  idx::SecondaryIndexDeltaLedger overlay_ledger;
  overlay_ledger.deltas.reserve(loaded_ledger.ledger.records.size());
  for (const auto& record : loaded_ledger.ledger.records) {
    if (!LedgerRecordRelevantToIndex(record, *selected, table_uuid)) {
      continue;
    }
    if (record.commit_state ==
            idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required ||
        record.commit_state == idx::SecondaryIndexDeltaLedgerCommitState::refused) {
      return RefuseIndexedLookup(
          "secondary_index_delta_ledger_record_refused");
    }
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned) {
      continue;
    }
    if (record.commit_state ==
        idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted &&
        record.delta.local_transaction_id != context.local_transaction_id) {
      continue;
    }
    overlay_ledger.deltas.push_back(record.delta);
    has_relevant_delta = true;
  }

  if (!has_relevant_delta) {
    result.rows = IndexedCrudRowsForPredicateForContext(
        state,
        table_uuid,
        predicate,
        context,
        limit,
        &result.index_evidence_id);
    result.ok = true;
    result.index_used = !result.index_evidence_id.empty();
    result.diagnostic = OkDiagnostic();
    result.evidence.push_back({"mga_secondary_index_lookup_path",
                               "non_unique_synchronous_no_delta"});
    return result;
  }

  if (!OverlayPredicateSupported(predicate)) {
    return RefuseIndexedLookup(
        "secondary_index_delta_overlay_predicate_unsupported");
  }

  std::vector<idx::SecondaryIndexBaseEntry> base_entries;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid != selected->index_uuid ||
        entry.table_uuid != table_uuid ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    idx::SecondaryIndexBaseEntry base;
    const auto diagnostic = BaseEntryForOverlay(entry, &base);
    if (diagnostic.error) {
      return RefuseIndexedLookup("secondary_index_base_entry_invalid",
                                 diagnostic);
    }
    base_entries.push_back(std::move(base));
  }

  idx::SecondaryIndexOverlayRequest overlay_request;
  auto diagnostic = ParseLedgerTypedUuid(selected->index_uuid,
                                         scratchbird::core::platform::UuidKind::object,
                                         &overlay_request.index_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_index_uuid_invalid",
                               diagnostic);
  }
  diagnostic = ParseLedgerTypedUuid(table_uuid,
                                    scratchbird::core::platform::UuidKind::object,
                                    &overlay_request.table_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_table_uuid_invalid",
                               diagnostic);
  }
  diagnostic = ParseLedgerTypedUuid(context.transaction_uuid.canonical,
                                    scratchbird::core::platform::UuidKind::transaction,
                                    &overlay_request.transaction_uuid);
  if (diagnostic.error) {
    return RefuseIndexedLookup("secondary_index_overlay_transaction_uuid_invalid",
                               diagnostic);
  }
  overlay_request.local_transaction_id = context.local_transaction_id;
  overlay_request.snapshot_high_water_local_transaction_id =
      SnapshotVisibleThroughForOverlay(state, context);
  overlay_request.index_kind = SecondaryIndexKindForCrudIndex(*selected);
  overlay_request.include_own_transaction = true;

  idx::SecondaryIndexOverlayLedger evidence_ledger;
  const auto overlay = idx::BuildSecondaryIndexDeltaOverlay(
      &evidence_ledger,
      base_entries,
      overlay_ledger,
      overlay_request);
  if (!overlay.ok()) {
    return RefuseIndexedLookup(
        "secondary_index_delta_overlay_refused",
        OverlayLookupDiagnostic(overlay.diagnostic,
                                "SB-MGA-SECONDARY-OVERLAY-REFUSED",
                                "mga.secondary_index_delta_overlay.refused"));
  }

  std::set<std::string> seen_candidates;
  std::size_t candidate_count = 0;
  for (const auto& entry : overlay.entries) {
    if (!OverlayEntryMatchesPredicate(entry, predicate)) {
      continue;
    }
    const std::string row_uuid =
        scratchbird::core::uuid::UuidToString(entry.row_uuid.value);
    if (!seen_candidates.insert(row_uuid).second) {
      continue;
    }
    ++candidate_count;
    const auto row = FindVisibleCrudRowForContext(
        state,
        table_uuid,
        row_uuid,
        context);
    if (row && CrudRowMatchesPredicate(*row, predicate)) {
      result.rows.push_back(*row);
      if (limit != 0 && result.rows.size() >= limit) {
        break;
      }
    }
  }
  result.index_evidence_id = CrudIndexEvidenceId(
      *selected,
      predicate,
      candidate_count,
      result.rows.size());
  result.ok = true;
  result.index_used = true;
  result.diagnostic = OkDiagnostic();
  result.evidence.push_back({"mga_secondary_index_lookup_path",
                             "non_unique_committed_delta_overlay"});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_used",
                             selected->index_uuid});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_visible_delta_count",
                             std::to_string(overlay.evidence.visible_delta_entries)});
  result.evidence.push_back({"mga_secondary_index_delta_overlay_result_count",
                             std::to_string(result.rows.size())});
  return result;
}

EngineApiDiagnostic AppendMgaIndexEntriesForIndex(const EngineRequestContext& context,
                                                  const CrudIndexRecord& index,
                                                  const std::string& row_uuid,
                                                  const std::string& version_uuid,
                                                  const std::vector<std::pair<std::string, std::string>>& values) {
  MgaIndexEntryAppendBatch batch;
  batch.index = index;
  batch.table_uuid = index.table_uuid;
  batch.rows.push_back(MgaIndexEntryRowInput{row_uuid, version_uuid, values});
  MgaRelationHotAppendContext append_context(context);
  const auto appended = append_context.AppendIndexEntryBatches({std::move(batch)});
  if (appended.error) { return appended; }
  return append_context.FlushIndexEntries();
}

EngineApiDiagnostic PersistMgaLargeValuesForRow(const EngineRequestContext& context,
                                                const std::string& table_uuid,
                                                const std::string& row_uuid,
                                                const std::string& version_uuid,
                                                bool force_large_value,
                                                std::vector<std::pair<std::string, std::string>>* values,
                                                std::vector<EngineEvidenceReference>* evidence) {
  MgaLargeValuePersistBatchCounters counters;
  return PersistMgaLargeValuesForRows(
      context,
      {MgaLargeValuePersistBatchRowInput{table_uuid,
                                         row_uuid,
                                         version_uuid,
                                         force_large_value,
                                         values}},
      &counters,
      evidence);
}

EngineApiDiagnostic PersistMgaLargeValuesForRows(
    const EngineRequestContext& context,
    const std::vector<MgaLargeValuePersistBatchRowInput>& rows,
    MgaLargeValuePersistBatchCounters* counters,
    std::vector<EngineEvidenceReference>* evidence) {
  MgaLargeValuePersistBatchCounters local_counters;
  if (counters == nullptr) {
    counters = &local_counters;
  }
  *counters = MgaLargeValuePersistBatchCounters{};
  counters->rows_seen = static_cast<std::uint64_t>(rows.size());

  struct PendingValueMutation {
    std::vector<std::pair<std::string, std::string>>* values = nullptr;
    std::vector<std::pair<std::string, std::string>> replacement_values;
  };

  std::vector<PendingValueMutation> pending_mutations;
  pending_mutations.reserve(rows.size());
  std::vector<EngineEvidenceReference> pending_evidence;
  std::vector<std::string> lines;

  for (const auto& row : rows) {
    if (row.values == nullptr) {
      return MakeInvalidRequestDiagnostic("mga.large_value", "values_required");
    }
    PendingValueMutation mutation;
    mutation.values = row.values;
    mutation.replacement_values = *row.values;
    bool force_one_remaining = row.force_large_value;
    while (EncodedValueBytes(mutation.replacement_values) >
               kCrudVerticalSliceMaxEncodedValueBytes ||
           force_one_remaining) {
      auto selected = mutation.replacement_values.end();
      for (auto it = mutation.replacement_values.begin();
           it != mutation.replacement_values.end();
           ++it) {
        if (it->second == "<NULL>" ||
            CrudValueIsLargeValueLocator(it->second) ||
            IsMgaLargeValueLocator(it->second)) {
          continue;
        }
        if (selected == mutation.replacement_values.end() ||
            it->second.size() > selected->second.size()) {
          selected = it;
        }
      }
      if (selected == mutation.replacement_values.end()) {
        return MakeInvalidRequestDiagnostic("mga.large_value",
                                            "no_overflow_candidate_available");
      }

      force_one_remaining = false;
      const std::string original = selected->second;
      const std::string overflow_uuid = GenerateCrudEngineUuid("object");
      const std::string content_hash = std::to_string(ChecksumText(original));
      const std::uint64_t total_bytes =
          static_cast<std::uint64_t>(original.size());
      lines.push_back(JoinLine({kRowStoreMagic,
                                "LARGE_VALUE",
                                std::to_string(context.local_transaction_id),
                                overflow_uuid,
                                row.table_uuid,
                                row.row_uuid,
                                row.version_uuid,
                                selected->first,
                                std::to_string(total_bytes),
                                content_hash,
                                "durable_uncommitted"}));
      ++counters->values_overflowed;
      counters->payload_bytes += total_bytes;

      std::uint64_t ordinal = 0;
      for (std::size_t offset = 0; offset < original.size();
           offset += kMgaLargeValueChunkBytes) {
        const std::size_t end =
            std::min<std::size_t>(original.size(),
                                  offset + kMgaLargeValueChunkBytes);
        const std::string fragment = original.substr(offset, end - offset);
        lines.push_back(JoinLine({kRowStoreMagic,
                                  "LARGE_VALUE_CHUNK",
                                  std::to_string(context.local_transaction_id),
                                  overflow_uuid,
                                  std::to_string(ordinal++),
                                  EncodeCrudText(fragment),
                                  std::to_string(ChecksumText(fragment))}));
        ++counters->chunks_appended;
      }
      selected->second = MakeMgaLargeValueLocator(overflow_uuid,
                                                  content_hash,
                                                  total_bytes);
      pending_evidence.push_back({"mga_large_value_overflow", overflow_uuid});
    }
    pending_mutations.push_back(std::move(mutation));
  }

  counters->preallocated_chunk_slots = counters->chunks_appended;
  counters->store_lines_appended = static_cast<std::uint64_t>(lines.size());
  if (!AppendLines(LargeValueStorePath(context),
                   lines,
                   &counters->stream_opens,
                   &counters->stream_flushes)) {
    return MakeInvalidRequestDiagnostic("mga.large_value",
                                        "large_value_batch_append_failed");
  }

  for (auto& mutation : pending_mutations) {
    *mutation.values = std::move(mutation.replacement_values);
  }

  if (evidence != nullptr && counters->values_overflowed != 0) {
    evidence->insert(evidence->end(),
                     pending_evidence.begin(),
                     pending_evidence.end());
    evidence->push_back({"mga_large_value_batch_writer", "window"});
    evidence->push_back({"mga_large_value_batch_rows",
                         std::to_string(counters->rows_seen)});
    evidence->push_back({"mga_large_value_batch_overflows",
                         std::to_string(counters->values_overflowed)});
    evidence->push_back({"mga_large_value_batch_chunks",
                         std::to_string(counters->chunks_appended)});
    evidence->push_back({"mga_large_value_batch_preallocated_chunks",
                         std::to_string(counters->preallocated_chunk_slots)});
    evidence->push_back({"mga_large_value_batch_payload_bytes",
                         std::to_string(counters->payload_bytes)});
    evidence->push_back({"mga_large_value_batch_stream_opens",
                         std::to_string(counters->stream_opens)});
    evidence->push_back({"mga_large_value_batch_stream_flushes",
                         std::to_string(counters->stream_flushes)});
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaLargeValueReclaimMarkersForRowVersion(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const CrudRowVersionRecord& row,
    const std::string& cleanup_reason,
    std::set<std::string>* already_reclaimed_overflow_uuids,
    std::uint64_t* reclaimed_count) {
  if (already_reclaimed_overflow_uuids == nullptr || reclaimed_count == nullptr) {
    return MakeInvalidRequestDiagnostic("mga.large_value", "reclaim_state_required");
  }
  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() < 11 || fields[0] != kRowStoreMagic ||
        fields[1] != "LARGE_VALUE" ||
        fields[4] != row.table_uuid ||
        fields[5] != row.row_uuid ||
        fields[6] != row.version_uuid) {
      continue;
    }
    const std::string& overflow_uuid = fields[3];
    if (!already_reclaimed_overflow_uuids->insert(overflow_uuid).second) {
      continue;
    }
    const std::string reclaim_line =
        JoinLine({kRowStoreMagic,
                  "LARGE_VALUE_RECLAIMED",
                  std::to_string(local_transaction_id),
                  overflow_uuid,
                  row.table_uuid,
                  row.row_uuid,
                  row.version_uuid,
                  fields[7],
                  cleanup_reason});
    if (!AppendLine(LargeValueStorePath(context), reclaim_line)) {
      return MakeInvalidRequestDiagnostic("mga.large_value",
                                          "large_value_reclaim_append_failed");
    }
    ++(*reclaimed_count);
  }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendMgaTemporaryTableMetadataRetirement(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const CrudTableRecord& table,
    const std::string& cleanup_reason) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "database_path_required");
  }
  const auto reservation = ReserveEventSequenceRange(
      context,
      "relation_metadata",
      MetadataStorePath(context),
      1,
      [&context]() { return ScanNextMetadataEventSequence(context); });
  if (!reservation.ok) { return reservation.diagnostic; }
  const std::string line = JoinLine({kRowStoreMagic,
                                     "TABLE_METADATA_RETIRED",
                                     std::to_string(local_transaction_id),
                                     std::to_string(reservation.first),
                                     table.table_uuid,
                                     cleanup_reason,
                                     table.temporary_session_uuid});
  if (!AppendLine(MetadataStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "table_metadata_retire_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ApplyMgaTemporaryCleanupActions(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    const std::string& cleanup_reason,
    bool include_delete_rows,
    bool include_preserve_rows,
    bool retire_private_metadata,
    std::uint64_t* deleted_row_count,
    std::uint64_t* reclaimed_large_value_count,
    std::uint64_t* retired_private_metadata_count) {
  if (deleted_row_count != nullptr) { *deleted_row_count = 0; }
  if (reclaimed_large_value_count != nullptr) { *reclaimed_large_value_count = 0; }
  if (retired_private_metadata_count != nullptr) {
    *retired_private_metadata_count = 0;
  }
  if (context.session_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "session_uuid_required");
  }
  if (local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("mga.temporary_session_cleanup",
                                        "local_transaction_id_required");
  }
  const auto cleanup_work = HasMgaTemporaryCleanupMetadataWork(context,
                                                              include_delete_rows,
                                                              include_preserve_rows,
                                                              retire_private_metadata);
  if (!cleanup_work.ok) { return cleanup_work.diagnostic; }
  if (!cleanup_work.has_work) { return OkDiagnostic(); }

  auto load_context = context;
  load_context.local_transaction_id = local_transaction_id;
  auto loaded = LoadMgaRelationStoreState(load_context);
  if (!loaded.ok) { return loaded.diagnostic; }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto visible_reclaims = LoadVisibleMgaLargeValueReclaims(context);
  if (visible_reclaims.diagnostic.error) { return visible_reclaims.diagnostic; }
  std::set<std::string> already_reclaimed_overflow_uuids =
      visible_reclaims.overflow_uuids;
  std::uint64_t deleted = 0;
  std::uint64_t reclaimed = 0;
  std::uint64_t retired_metadata = 0;
  for (const auto& table : state.tables) {
    if (!table.temporary) { continue; }
    const bool delete_rows_policy = table.on_commit_action == "delete_rows";
    const bool preserve_rows_policy = table.on_commit_action == "preserve_rows";
    if ((delete_rows_policy && !include_delete_rows) ||
        (preserve_rows_policy && !include_preserve_rows) ||
        (!delete_rows_policy && !preserve_rows_policy)) {
      continue;
    }
    auto row_context = context;
    row_context.local_transaction_id = local_transaction_id;
    const auto rows =
        VisibleCrudRowsForContext(state, table.table_uuid, row_context);
    std::set<std::string> visible_row_uuids;
    for (const auto& row : rows) { visible_row_uuids.insert(row.row_uuid); }
    for (const auto& row_version : state.row_versions) {
      if (row_version.table_uuid != table.table_uuid ||
          visible_row_uuids.count(row_version.row_uuid) == 0 ||
          !CrudRowVersionVisibleToContext(state, row_version, row_context)) {
        continue;
      }
      const auto reclaimed_large = AppendMgaLargeValueReclaimMarkersForRowVersion(
          context,
          local_transaction_id,
          row_version,
          cleanup_reason,
          &already_reclaimed_overflow_uuids,
          &reclaimed);
      if (reclaimed_large.error) { return reclaimed_large; }
    }
    for (const auto& row : rows) {
      CrudRowVersionRecord tombstone;
      tombstone.creator_tx = local_transaction_id;
      tombstone.table_uuid = table.table_uuid;
      tombstone.row_uuid = row.row_uuid;
      tombstone.version_uuid = GenerateCrudEngineUuid("row");
      tombstone.temporary_session_uuid = row.temporary_session_uuid;
      tombstone.previous_version_uuid = row.version_uuid;
      tombstone.previous_sequence = row.sequence;
      tombstone.deleted = true;
      const auto appended = AppendMgaRowVersion(context, tombstone, nullptr);
      if (appended.error) { return appended; }
      ++deleted;
    }
    if (retire_private_metadata &&
        table.temporary_scope != "global" &&
        table.temporary_session_uuid == context.session_uuid.canonical) {
      const auto retired = AppendMgaTemporaryTableMetadataRetirement(
          context,
          local_transaction_id,
          table,
          cleanup_reason);
      if (retired.error) { return retired; }
      ++retired_metadata;
    }
  }
  if (deleted_row_count != nullptr) { *deleted_row_count = deleted; }
  if (reclaimed_large_value_count != nullptr) {
    *reclaimed_large_value_count = reclaimed;
  }
  if (retired_private_metadata_count != nullptr) {
    *retired_private_metadata_count = retired_metadata;
  }
  return OkDiagnostic();
}

EngineApiDiagnostic CreateMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic("transaction.create_savepoint", "local_transaction_id_required");
  }
  const std::uint64_t row_cutoff = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_cutoff = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_cutoff = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(row_cutoff),
                                     std::to_string(metadata_cutoff),
                                     std::to_string(index_cutoff)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.create_savepoint", "savepoint_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ReleaseMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto exists = ValidateMgaSavepointExists(context, savepoint_name, "transaction.release_savepoint");
  if (exists.error) { return exists; }
  const std::uint64_t row_cutoff = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_cutoff = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_cutoff = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "RELEASE_SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(row_cutoff),
                                     std::to_string(metadata_cutoff),
                                     std::to_string(index_cutoff)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.release_savepoint", "savepoint_release_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic RollbackToMgaSavepointMarker(const EngineRequestContext& context, const std::string& savepoint_name) {
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end()) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_not_found");
  }
  const auto savepoint_it = tx_it->second.find(savepoint_name);
  if (savepoint_it == tx_it->second.end()) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_not_found");
  }
  const std::uint64_t row_upper = NextRowEventSequence(context) - 1;
  const std::uint64_t metadata_upper = NextMetadataEventSequence(context) - 1;
  const std::uint64_t index_upper = NextIndexEventSequence(context) - 1;
  const std::string line = JoinLine({kRowStoreMagic,
                                     "ROLLBACK_TO_SAVEPOINT",
                                     std::to_string(context.local_transaction_id),
                                     EncodeCrudText(savepoint_name),
                                     std::to_string(savepoint_it->second.row_event_sequence),
                                     std::to_string(savepoint_it->second.metadata_event_sequence),
                                     std::to_string(savepoint_it->second.index_event_sequence),
                                     std::to_string(row_upper),
                                     std::to_string(metadata_upper),
                                     std::to_string(index_upper)});
  if (!AppendLine(SavepointStorePath(context), line)) {
    return MakeInvalidRequestDiagnostic("transaction.rollback_to_savepoint", "savepoint_rollback_append_failed");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateMgaSavepointExists(const EngineRequestContext& context,
                                               const std::string& savepoint_name,
                                               const std::string& operation_id) {
  if (context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end() ||
      tx_it->second.find(savepoint_name) == tx_it->second.end()) {
    return MakeInvalidRequestDiagnostic(operation_id, "savepoint_not_found");
  }
  return OkDiagnostic();
}

std::vector<std::string> ActiveMgaSavepointNames(const EngineRequestContext& context) {
  std::vector<std::string> names;
  if (context.local_transaction_id == 0 || context.database_path.empty()) {
    return names;
  }
  const auto savepoints = ParseSavepoints(context);
  const auto tx_it = savepoints.active_savepoints.find(context.local_transaction_id);
  if (tx_it == savepoints.active_savepoints.end()) {
    return names;
  }
  names.reserve(tx_it->second.size());
  for (const auto& entry : tx_it->second) {
    names.push_back(entry.first);
  }
  return names;
}

EngineApiDiagnostic ApplyMgaTemporaryOnCommitActions(const EngineRequestContext& context,
                                                     std::uint64_t local_transaction_id,
                                                     std::uint64_t* deleted_row_count,
                                                     std::uint64_t* reclaimed_large_value_count) {
  return ApplyMgaTemporaryCleanupActions(context,
                                         local_transaction_id,
                                         "temporary_on_commit_delete_rows",
                                         true,
                                         false,
                                         false,
                                         deleted_row_count,
                                         reclaimed_large_value_count,
                                         nullptr);
}

EngineApiDiagnostic ApplyMgaTemporarySessionCleanupActions(
    const EngineRequestContext& context,
    std::uint64_t local_transaction_id,
    std::uint64_t* deleted_row_count,
    std::uint64_t* reclaimed_large_value_count,
    std::uint64_t* retired_private_metadata_count) {
  return ApplyMgaTemporaryCleanupActions(context,
                                         local_transaction_id,
                                         "temporary_session_cleanup",
                                         true,
                                         true,
                                         true,
                                         deleted_row_count,
                                         reclaimed_large_value_count,
                                         retired_private_metadata_count);
}

MgaTemporaryTableDropResult DropMgaTemporaryTable(
    const EngineRequestContext& context,
    const std::string& table_uuid) {
  MgaTemporaryTableDropResult result;
  if (table_uuid.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("ddl.drop_object",
                                                     "target_table_uuid_required");
    return result;
  }
  const auto visibility = CheckMgaTemporaryTableVisibility(context, table_uuid);
  if (!visibility.ok) {
    result.diagnostic = visibility.diagnostic;
    return result;
  }
  if (!visibility.known_temporary) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    return result;
  }
  result.target_was_temporary = true;
  if (context.session_uuid.canonical.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "temporary_table_requires_session_uuid");
    return result;
  }
  if (!visibility.table_visible || !visibility.visible_to_session) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "target_temporary_table_not_visible");
    return result;
  }
  if (context.local_transaction_id == 0) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "local_transaction_id_required");
    return result;
  }

  const auto authority =
      ValidateMgaMutatingTransactionAuthority(context, "ddl.drop_object");
  if (authority.error) {
    result.diagnostic = authority;
    return result;
  }

  auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(
      state,
      table_uuid,
      context.local_transaction_id);
  if (!table || !table->temporary) {
    result.diagnostic = MakeInvalidRequestDiagnostic(
        "ddl.drop_object",
        "target_temporary_table_not_visible");
    return result;
  }

  const auto visible_reclaims = LoadVisibleMgaLargeValueReclaims(context);
  if (visible_reclaims.diagnostic.error) {
    result.diagnostic = visible_reclaims.diagnostic;
    return result;
  }
  std::set<std::string> already_reclaimed_overflow_uuids =
      visible_reclaims.overflow_uuids;

  auto row_context = context;
  const auto rows = VisibleCrudRowsForContext(state, table_uuid, row_context);
  std::set<std::string> visible_row_uuids;
  for (const auto& row : rows) { visible_row_uuids.insert(row.row_uuid); }
  for (const auto& row_version : state.row_versions) {
    if (row_version.table_uuid != table_uuid ||
        visible_row_uuids.count(row_version.row_uuid) == 0 ||
        !CrudRowVersionVisibleToContext(state, row_version, row_context)) {
      continue;
    }
    const auto reclaimed_large = AppendMgaLargeValueReclaimMarkersForRowVersion(
        context,
        context.local_transaction_id,
        row_version,
        "temporary_table_drop",
        &already_reclaimed_overflow_uuids,
        &result.reclaimed_large_value_count);
    if (reclaimed_large.error) {
      result.diagnostic = reclaimed_large;
      return result;
    }
  }
  for (const auto& row : rows) {
    CrudRowVersionRecord tombstone;
    tombstone.creator_tx = context.local_transaction_id;
    tombstone.table_uuid = row.table_uuid;
    tombstone.row_uuid = row.row_uuid;
    tombstone.version_uuid = GenerateCrudEngineUuid("row");
    tombstone.temporary_session_uuid = row.temporary_session_uuid;
    tombstone.previous_version_uuid = row.version_uuid;
    tombstone.previous_sequence = row.sequence;
    tombstone.deleted = true;
    const auto appended = AppendMgaRowVersion(context, tombstone, nullptr);
    if (appended.error) {
      result.diagnostic = appended;
      return result;
    }
    ++result.deleted_row_count;
  }

  const auto retired = AppendMgaTemporaryTableMetadataRetirement(
      context,
      context.local_transaction_id,
      *table,
      "temporary_table_drop");
  if (retired.error) {
    result.diagnostic = retired;
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.metadata_retired = true;
  result.temporary_scope = table->temporary_scope;
  return result;
}

void ClearMgaEventSequenceRangeCacheForTesting() {
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  EventSequenceCache().clear();
}

MgaEventSequenceRangeReservation ReserveMgaRowEventSequenceRangeForTesting(
    const EngineRequestContext& context,
    std::uint64_t count) {
  return ReserveEventSequenceRange(
      context,
      "row_versions",
      RowStorePath(context),
      count,
      [&context]() { return ScanNextRowEventSequence(context); });
}

}  // namespace scratchbird::engine::internal_api

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic HeapAcquisitionRefusal(std::string code,
                                                   std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

PhysicalMgaStatementContext PhysicalMgaContextFromResolvedSnapshot(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor) {
  PhysicalMgaStatementContext expected;
  expected.statement_uuid = context.statement_uuid.canonical;
  expected.owning_transaction_uuid = context.transaction_uuid.canonical;
  expected.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
  expected.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  expected.owning_local_transaction_id = descriptor.owning_transaction.value;
  expected.visible_committed_high_watermark =
      descriptor.visible_committed_high_watermark;
  expected.oldest_active_transaction_id =
      descriptor.oldest_active_transaction.value;
  expected.oldest_interesting_transaction_id =
      descriptor.oldest_interesting_transaction.value;
  expected.oldest_snapshot_transaction_id =
      descriptor.oldest_snapshot_transaction.value;
  expected.retention_horizon_transaction_id =
      descriptor.retention_horizon_transaction.value;
  expected.active_excluded_local_transaction_ids =
      descriptor.active_excluded_local_transaction_ids;
  expected.in_doubt_excluded_local_transaction_ids =
      descriptor.in_doubt_excluded_local_transaction_ids;
  expected.snapshot_kind =
      scratchbird::transaction::mga::SnapshotVectorKindName(
          descriptor.snapshot_kind);
  expected.publication_inventory_next_local_transaction_id =
      descriptor.publication_inventory_next_local_transaction_id;
  expected.inventory_authoritative = descriptor.inventory_authoritative;
  expected.complete = descriptor.complete;
  expected.current = true;
  return expected;
}

DescriptorRuntimeDiagnostic ValidateCurrentHeapPhysicalMgaAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const TypedPhysicalNodeDag& physical_dag) {
  namespace api = scratchbird::engine::internal_api;
  api::EngineResolveStatementSnapshotRequest resolve_request;
  resolve_request.context = context;
  const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
  if (!resolved.ok) {
    return HeapAcquisitionRefusal(
        "SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
        "statement snapshot is unknown, revoked, stale, or not current");
  }
  const auto expected = PhysicalMgaContextFromResolvedSnapshot(
      context, resolved.snapshot_vector);
  if (!PhysicalMgaStatementContextEqual(physical_dag.mga_statement_context,
                                        expected)) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
        "physical MGA statement context differs from current inventory authority");
  }
  return {};
}

CanonicalExecutionMgaAuthority BuildCurrentHeapExecutionMgaAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const TypedPhysicalNodeDag& physical_dag) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalExecutionMgaAuthority authority;
  authority.statement_context = physical_dag.mga_statement_context;
  authority.origin = CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  authority.resolve_current = [context]() {
    CanonicalMgaCurrentResolution current;
    api::EngineResolveStatementSnapshotRequest resolve_request;
    resolve_request.context = context;
    const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
    if (!resolved.ok) {
      current.diagnostic = HeapAcquisitionRefusal(
          "SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
          "statement snapshot is unknown, revoked, stale, or not current");
      return current;
    }
    current.statement_context = PhysicalMgaContextFromResolvedSnapshot(
        context, resolved.snapshot_vector);
    return current;
  };
  return authority;
}

bool IsCanonicalHeapBindingUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) { continue; }
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) { return false; }
  }
  return true;
}

std::optional<std::string> ExactHeapDescriptorField(
    const std::string_view descriptor,
    const std::string_view field_name) {
  const std::string prefix = std::string(field_name) + "=";
  std::optional<std::string> value;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      if (value.has_value() || field.size() == prefix.size()) {
        return std::nullopt;
      }
      value = std::string(field.substr(prefix.size()));
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  return value;
}

bool CheckedHeapBoundToU64(const std::size_t value, std::uint64_t* output) {
  if (output == nullptr ||
      value > static_cast<std::size_t>(
                  std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  *output = static_cast<std::uint64_t>(value);
  return true;
}

constexpr std::size_t kMaximumHeapOutputColumns = 4096;

bool CheckedHeapCellCount(const std::size_t row_count,
                          const std::size_t column_count,
                          std::size_t* output) {
  if (output == nullptr ||
      (column_count != 0 &&
       row_count > std::numeric_limits<std::size_t>::max() / column_count)) {
    return false;
  }
  *output = row_count * column_count;
  return true;
}

bool ExactOptionalHeapDescriptorFieldMatches(
    const std::string_view descriptor,
    const std::string_view field_name,
    const std::optional<std::string>& expected) {
  const std::string prefix = std::string(field_name) + "=";
  std::size_t matches = 0;
  std::string_view actual;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      ++matches;
      actual = field.substr(prefix.size());
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  if (!expected.has_value()) { return matches == 0; }
  return matches == 1 && !actual.empty() && actual == *expected;
}

bool ExactHeapNullabilityCarrierMatches(const std::string_view descriptor,
                                        const bool expected_nullable) {
  std::optional<bool> admitted;
  std::size_t canonical_count = 0;
  std::size_t storage_count = 0;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    std::optional<bool> current;
    if (field.rfind("nullability=", 0) == 0) {
      ++canonical_count;
      const auto value = field.substr(std::string_view("nullability=").size());
      if (value == "nullable") {
        current = true;
      } else if (value == "non_null") {
        current = false;
      } else {
        return false;
      }
    } else if (field.rfind("nullable=", 0) == 0) {
      ++storage_count;
      const auto value = field.substr(std::string_view("nullable=").size());
      if (value == "true") {
        current = true;
      } else if (value == "false") {
        current = false;
      } else {
        return false;
      }
    }
    if (current.has_value()) {
      if (admitted.has_value() && *admitted != *current) { return false; }
      admitted = current;
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  return canonical_count <= 1 && storage_count <= 1 &&
         admitted.has_value() && *admitted == expected_nullable;
}

}  // namespace

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
CanonicalHeapRelationAcquisitionResult ExecuteCanonicalHeapRelationAcquisition(
    const CanonicalHeapRelationAcquisitionRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalHeapRelationAcquisitionResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool data_access_observed = false,
                          const bool cancellation_observed = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.data_access_observed = data_access_observed;
    result.cancellation_observed = cancellation_observed;
    return result;
  };
  const auto invalid = [&](std::string code,
                           std::string detail,
                           const bool data_access_observed = false,
                           const bool cancellation_observed = false) {
    return refuse(HeapAcquisitionRefusal(std::move(code), std::move(detail)),
                  data_access_observed,
                  cancellation_observed);
  };

  if (request.context == nullptr || request.relational_dag == nullptr) {
    return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                   "engine context and typed relational DAG are required");
  }
  const auto& context = *request.context;
  const auto& relational = *request.relational_dag;
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  if (request.mga_authority.origin !=
      CanonicalMgaAuthorityOrigin::kEngineTransactionInventory) {
    return invalid("QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
                   "heap acquisition requires engine transaction-inventory authority");
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!entry_authority.ok) return refuse(entry_authority);
  const auto relational_validation = api::ValidateTypedRelationalDag(relational);
  if (!relational_validation.accepted) {
    const auto& issue = relational_validation.issues.front();
    return invalid(issue.diagnostic_id,
                   issue.field_id + ":node_id=" +
                       std::to_string(issue.node_id));
  }
  const auto physical_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!physical_validation.accepted) {
    const auto& issue = physical_validation.issues.front();
    return invalid(issue.diagnostic_id, issue.field_id);
  }
  const auto current_mga =
      ValidateCurrentHeapPhysicalMgaAuthority(context, request.physical_dag);
  if (!current_mga.ok) {
    return refuse(current_mga);
  }
  if (relational.wire_version != 2 ||
      request.physical_dag.abi_version != 2 ||
      !request.physical_dag.optimizer_published ||
      !request.physical_dag.immutable_node_identity_validated ||
      !request.physical_dag.capability_validated_before_access ||
      request.physical_dag.data_access_observed) {
    return invalid("QOW-DIAG-QRY-004-HEAP-DIRECT-SCOPE-V1",
                   "wire-v2 direct optimizer publication is required");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return invalid("QOW-DIAG-QRY-004-HEAP-DIRECT-SCOPE-V1",
                   "prepared or cached descriptor execution is outside this profile");
  }
  if (!request.cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > kMaximumHeapOutputColumns) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "valid nonzero heap row, column, cell, byte, scan, and "
                   "cancellation bounds are required");
  }
  if (request.cancellation_requested()) {
    return invalid("QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
                   "heap relation acquisition cancelled before admission",
                   false,
                   true);
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      request.physical_dag.local_transaction_id !=
          context.local_transaction_id ||
      request.physical_dag.statement_snapshot_id !=
          context.snapshot_visible_through_local_transaction_id) {
    return invalid("SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
                   "physical scan does not match the active MGA transaction snapshot");
  }
  const auto& authorization = context.authorization_context;
  if (!context.statement_metadata_snapshot_engine_owned ||
      !context.security_context_present || !authorization.present ||
      relational.bound_sblr_tree_uuid !=
          request.physical_dag.bound_sblr_tree_uuid ||
      relational.statement_uuid != context.statement_uuid.canonical ||
      relational.owning_transaction_uuid !=
          context.transaction_uuid.canonical ||
      relational.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      relational.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      relational.local_transaction_id != context.local_transaction_id ||
      relational.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id ||
      relational.bound_catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      request.physical_dag.catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      relational.bound_security_context_uuid !=
          authorization.authority_uuid.canonical ||
      request.physical_dag.security_context_uuid !=
          authorization.authority_uuid.canonical ||
      request.physical_dag.catalog_generation !=
          context.catalog_generation_id ||
      context.catalog_generation_id !=
          authorization.catalog_generation_id ||
      request.physical_dag.security_epoch != context.security_epoch ||
      context.security_epoch != authorization.security_epoch ||
      request.physical_dag.policy_epoch != authorization.policy_epoch ||
      request.physical_dag.resource_epoch != context.resource_epoch ||
      request.physical_dag.capability_snapshot_uuid !=
          context.optimizer_capability_snapshot_uuid.canonical ||
      request.physical_dag.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      request.physical_dag.route_snapshot_uuid !=
          context.optimizer_route_snapshot_uuid.canonical ||
      request.physical_dag.route_epoch != context.optimizer_route_epoch ||
      request.physical_dag.route_generation !=
          context.optimizer_route_generation ||
      request.physical_dag.memory_budget_bytes !=
          context.optimizer_memory_budget_bytes) {
    return invalid("QOW-DIAG-QRY-004-HEAP-AUTHORITY-SCOPE-V1",
                   "catalog, security, resource, or MGA scope is stale or mismatched");
  }
  if (context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      request.maximum_decoded_bytes > context.optimizer_memory_budget_bytes ||
      request.maximum_scanned_row_versions >
          context.optimizer_maximum_candidate_count ||
      request.maximum_output_rows >
          context.optimizer_maximum_candidate_count) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap scan bounds exceed the admitted optimizer resources");
  }

  const auto physical = std::ranges::find_if(
      request.physical_dag.nodes, [&](const auto& node) {
        return node.physical_node_id == request.selected_physical_node_id;
      });
  if (request.selected_physical_node_id == 0 ||
      physical == request.physical_dag.nodes.end() ||
      physical->node_kind != PhysicalNodeKind::kScan ||
      physical->implementation_id != "scan.heap.v1" ||
      !physical->input_physical_node_ids.empty() ||
      !physical->engine_capability_validated) {
    return invalid("QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
                   "selected physical node is not an admitted leaf heap scan");
  }
  const auto relation_node = std::ranges::find_if(
      relational.nodes, [&](const auto& node) {
        return node.node_id == physical->relational_node_id;
      });
  if (relation_node == relational.nodes.end() ||
      relation_node->node_kind != api::RelationalDagNodeKind::kScan ||
      !relation_node->input_node_ids.empty() ||
      relation_node->semantic_variant_id != "relation.source.v1" ||
      relation_node->required_object_uuids.size() != 1 ||
      relation_node->output_descriptor_ids.empty() ||
      relation_node->output_descriptor_ids.size() !=
          relation_node->bound_expression_ids.size() ||
      relation_node->output_descriptor_ids.size() >
          request.maximum_output_columns ||
      relation_node->output_descriptor_ids.size() >
          kMaximumHeapOutputColumns ||
      physical->output_descriptor_ids != relation_node->output_descriptor_ids) {
    return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                   "selected relation source binding is not exact");
  }
  const std::string& relation_uuid =
      relation_node->required_object_uuids.front();
  if (!IsCanonicalHeapBindingUuid(relation_uuid)) {
    return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                   "bound relation UUID is not canonical");
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : relational.outputs) {
    if (output.relation_node_id == relation_node->node_id) {
      outputs.push_back(&output);
    }
  }
  const auto output_width = relation_node->output_descriptor_ids.size();
  if (outputs.size() != output_width ||
      relational.outputs.size() != output_width ||
      relational.expressions.size() != output_width ||
      relational.descriptors.size() != output_width) {
    return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                   "relation source must cover its complete visible width");
  }
  std::size_t admitted_shape_cells = 0;
  if (!CheckedHeapCellCount(request.maximum_output_rows, output_width,
                            &admitted_shape_cells)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "admitted heap row-by-width shape overflows size_t");
  }

  // QOW-SOURCE-QRY-004-HEAP-PROJECTED-WIDTH-V1
  // The leaf scan accepts one visible, one-to-one mapping in requested output
  // order from optimizer outputs through identifier bindings and descriptors.
  // Persisted column UUIDs select a bounded subset; aliases, hidden outputs,
  // and duplicate bindings remain outside this profile.
  std::vector<const api::RelationalExpressionRecord*> expressions;
  std::vector<const api::RelationalTypeDescriptor*> relational_descriptors;
  expressions.reserve(output_width);
  relational_descriptors.reserve(output_width);
  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> expression_ids;
  std::unordered_set<std::uint32_t> descriptor_ids;
  std::unordered_set<std::string> bound_column_uuids;
  for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
    const auto& output = *outputs[ordinal];
    const auto expression = relational.expressions.begin() + ordinal;
    const auto descriptor = relational.descriptors.begin() + ordinal;
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        !output_ids.insert(output.output_id).second ||
        output.output_name_utf8.empty() ||
        output.descriptor_id != relation_node->output_descriptor_ids[ordinal] ||
        expression->expression_id != output.expression_id ||
        !expression_ids.insert(expression->expression_id).second ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        !IsCanonicalHeapBindingUuid(*expression->bound_name_uuid) ||
        !bound_column_uuids.insert(*expression->bound_name_uuid).second ||
        expression->result_descriptor_id != output.descriptor_id ||
        relation_node->bound_expression_ids[ordinal] !=
            expression->expression_id ||
        descriptor->descriptor_id != output.descriptor_id ||
        !descriptor_ids.insert(descriptor->descriptor_id).second ||
        !IsCanonicalHeapBindingUuid(descriptor->descriptor_uuid) ||
        !IsCanonicalHeapBindingUuid(descriptor->type_uuid) ||
        descriptor->nullability == api::RelationalNullability::kUnknown ||
        (descriptor->collation_uuid.has_value() &&
         !IsCanonicalHeapBindingUuid(*descriptor->collation_uuid)) ||
        (descriptor->timezone_profile_id.has_value() &&
         descriptor->timezone_profile_id->empty())) {
      return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                     "relation output binding is incomplete or not bijective");
    }
    expressions.push_back(&*expression);
    relational_descriptors.push_back(&*descriptor);
  }

  const auto authorization_decision = api::EvaluateMaterializedAuthorization(
      context, authorization, "SELECT", relation_uuid);
  if (!authorization_decision.authorized || authorization_decision.denied ||
      authorization_decision.policy_recheck_required ||
      !authorization_decision.diagnostics.empty()) {
    const std::string detail = authorization_decision.diagnostics.empty()
                                   ? "SELECT authorization is indeterminate"
                                   : authorization_decision.diagnostics.front().detail;
    return invalid("QOW-DIAG-QRY-004-SCAN-SECURITY-DECISION-V1", detail);
  }

  std::uint64_t maximum_scanned = 0;
  std::uint64_t maximum_bytes = 0;
  std::uint64_t maximum_output = 0;
  if (!CheckedHeapBoundToU64(request.maximum_scanned_row_versions,
                             &maximum_scanned) ||
      !CheckedHeapBoundToU64(request.maximum_decoded_bytes, &maximum_bytes) ||
      !CheckedHeapBoundToU64(request.maximum_output_rows, &maximum_output)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap scan bound conversion overflowed");
  }
  api::MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = relation_uuid;
  read_request.maximum_scanned_row_versions = maximum_scanned;
  read_request.maximum_decoded_bytes = maximum_bytes;
  read_request.maximum_output_rows = maximum_output;
  read_request.cancellation_requested = request.cancellation_requested;
  const auto read = api::ReadVisibleMgaHeapRelation(context, read_request);
  if (!read.ok) {
    return invalid(read.diagnostic.code.empty()
                       ? "QOW-DIAG-QRY-004-HEAP-READ-V1"
                       : read.diagnostic.code,
                   read.diagnostic.detail,
                   read.scanned_row_version_count != 0 ||
                       read.decoded_byte_count != 0,
                   read.cancellation_observed);
  }

  if (read.descriptor.columns.empty() ||
      read.descriptor.columns.size() < output_width ||
      read.descriptor.columns.size() > kMaximumHeapOutputColumns) {
    return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                   "persisted relation width cannot satisfy the projected binding",
                   true);
  }
  std::size_t materialized_cell_count = 0;
  if (!CheckedHeapCellCount(read.visible_rows.size(), output_width,
                            &materialized_cell_count) ||
      materialized_cell_count > request.maximum_output_cells) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap materialized cell bound would be exceeded",
                   true);
  }

  DescriptorBatch batch;
  std::vector<api::EngineDescriptor> output_descriptors;
  std::vector<std::string> column_uuids;
  std::unordered_set<std::string> persisted_column_uuids;
  std::unordered_set<std::string> persisted_column_names;
  std::vector<const api::MgaRelationColumnStorageDescriptor*>
      projected_columns;
  for (std::size_t persisted_ordinal = 0;
       persisted_ordinal < read.descriptor.columns.size();
       ++persisted_ordinal) {
    const auto& column = read.descriptor.columns[persisted_ordinal];
    if (column.ordinal != persisted_ordinal ||
        !IsCanonicalHeapBindingUuid(column.column_uuid.canonical) ||
        !persisted_column_uuids.insert(column.column_uuid.canonical).second ||
        column.canonical_name_key.empty() ||
        !persisted_column_names.insert(column.canonical_name_key).second) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "persisted relation column identities are incomplete",
                     true);
    }
  }
  output_descriptors.reserve(output_width);
  column_uuids.reserve(output_width);
  projected_columns.reserve(output_width);
  batch.columns.reserve(output_width);
  for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
    const auto& output = *outputs[ordinal];
    const auto& expression = *expressions[ordinal];
    const auto& relational_descriptor = *relational_descriptors[ordinal];
    const auto persisted_column = std::ranges::find_if(
        read.descriptor.columns, [&](const auto& candidate) {
          return candidate.column_uuid.canonical ==
                 *expression.bound_name_uuid;
        });
    if (persisted_column == read.descriptor.columns.end()) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "projected column UUID is absent from persisted relation",
                     true);
    }
    const auto& column = *persisted_column;
    const auto persisted_type_uuid = ExactHeapDescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const bool nullable = relational_descriptor.nullability ==
                          api::RelationalNullability::kNullable;
    if (column.column_uuid.canonical != *expression.bound_name_uuid ||
        output.output_name_utf8 != column.canonical_name_key ||
        column.value_descriptor.descriptor_uuid.canonical !=
            relational_descriptor.descriptor_uuid ||
        column.value_descriptor.encoded_descriptor.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        !persisted_type_uuid.has_value() ||
        !IsCanonicalHeapBindingUuid(*persisted_type_uuid) ||
        *persisted_type_uuid != relational_descriptor.type_uuid ||
        nullable != column.nullable ||
        !ExactHeapNullabilityCarrierMatches(
            column.value_descriptor.encoded_descriptor, nullable) ||
        (relational_descriptor.collation_uuid.has_value()
             ? column.collation_uuid !=
                   *relational_descriptor.collation_uuid
             : !column.collation_uuid.empty()) ||
        !ExactOptionalHeapDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "collation_uuid", relational_descriptor.collation_uuid) ||
        !ExactOptionalHeapDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "timezone_profile_id",
            relational_descriptor.timezone_profile_id)) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "persisted ordinal column descriptor differs from binding",
                     true);
    }
    projected_columns.push_back(&column);
    api::EngineDescriptor output_descriptor;
    output_descriptor.descriptor_uuid =
        column.value_descriptor.descriptor_uuid;
    output_descriptor.descriptor_kind = "scalar";
    output_descriptor.canonical_type_name =
        column.value_descriptor.canonical_type_name;
    output_descriptor.encoded_descriptor =
        "type_uuid=" + relational_descriptor.type_uuid +
        ";nullability=" + (nullable ? "nullable" : "non_null");
    if (relational_descriptor.collation_uuid.has_value()) {
      output_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *relational_descriptor.collation_uuid;
    }
    if (relational_descriptor.timezone_profile_id.has_value()) {
      output_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" +
          *relational_descriptor.timezone_profile_id;
    }
    if (relational_descriptor.width.has_value()) {
      output_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*relational_descriptor.width);
    }
    if (relational_descriptor.precision.has_value()) {
      output_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*relational_descriptor.precision);
    }
    if (relational_descriptor.scale.has_value()) {
      output_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*relational_descriptor.scale);
    }
    batch.columns.push_back({column.canonical_name_key,
                             output_descriptor,
                             column.nullable,
                             output.descriptor_id});
    output_descriptors.push_back(std::move(output_descriptor));
    column_uuids.push_back(column.column_uuid.canonical);
  }
  batch.rows.reserve(read.visible_rows.size());
  std::vector<std::string> record_uuids;
  std::vector<std::string> version_uuids;
  record_uuids.reserve(read.visible_rows.size());
  version_uuids.reserve(read.visible_rows.size());
  for (std::size_t row_index = 0; row_index < read.visible_rows.size();
       ++row_index) {
    if (request.cancellation_requested()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
                     "heap relation acquisition cancelled during materialization",
                     true,
                     true);
    }
    const auto& stored_row = read.visible_rows[row_index];
    if (stored_row.values.size() != read.descriptor.columns.size()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                     "stored row width differs from persisted descriptor width",
                     true);
    }
    DescriptorTuple tuple;
    tuple.values.reserve(output_width);
    for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
      const auto& column = *projected_columns[ordinal];
      const std::string* encoded_value = nullptr;
      for (const auto& [name, value] : stored_row.values) {
        if (name != column.canonical_name_key) { continue; }
        if (encoded_value != nullptr) {
          return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                         "stored row contains a duplicate persisted column",
                         true);
        }
        encoded_value = &value;
      }
      if (encoded_value == nullptr) {
        return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                       "stored row omits a persisted column",
                       true);
      }
      api::EngineTypedValue value;
      value.descriptor = output_descriptors[ordinal];
      if (*encoded_value == "<NULL>") {
        value.is_null = true;
        value.state = api::EngineValueState::sql_null;
      } else {
        value.encoded_value = *encoded_value;
        value.state = api::EngineValueState::value;
      }
      tuple.values.push_back(std::move(value));
    }
    batch.rows.push_back(std::move(tuple));
    record_uuids.push_back(stored_row.row_uuid);
    version_uuids.push_back(stored_row.version_uuid);
  }
  const auto batch_validation =
      ValidateCanonicalDescriptorBatch(batch, physical->output_descriptor_ids);
  if (!batch_validation.ok) {
    return invalid(batch_validation.diagnostic_code,
                   batch_validation.detail,
                   true);
  }
  const auto value_validation = ValidateDescriptorBatch(batch);
  if (!value_validation.ok) {
    return invalid(value_validation.diagnostic_code,
                   value_validation.detail,
                   true);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority, true);

  result.diagnostic = {};
  result.output_batch = std::move(batch);
  result.emitted_record_uuids = std::move(record_uuids);
  result.emitted_row_version_uuids = std::move(version_uuids);
  result.counters.scanned_row_version_count =
      read.scanned_row_version_count;
  result.counters.decoded_byte_count = read.decoded_byte_count;
  result.counters.visibility_recheck_count = read.visibility_recheck_count;
  result.counters.invisible_row_version_count =
      read.invisible_row_version_count;
  result.counters.tombstone_row_count = read.tombstone_row_count;
  result.counters.emitted_row_count = result.output_batch.rows.size();
  result.counters.output_column_count = output_width;
  result.counters.materialized_cell_count = materialized_cell_count;
  result.authority.engine_catalog_descriptor_loaded = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.engine_authorization_rechecked = true;
  result.authority.bounded_physical_read = true;
  result.data_access_observed = read.scoped_physical_segment_used;
  result.relation_uuid = relation_uuid;
  result.column_uuids = std::move(column_uuids);
  result.current_relation_descriptor_uuid =
      read.descriptor.descriptor_uuid.canonical;
  result.current_relation_descriptor_generation =
      read.descriptor.descriptor_generation;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = physical->physical_node_id;
  result.causal_counter_id = physical->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

namespace {

struct HeapPhysicalDispatchObservation {
  bool callback_invoked = false;
  bool data_access_observed = false;
  bool cancellation_observed = false;
};

struct HeapPhysicalExecutorState {
  scratchbird::engine::internal_api::EngineRequestContext context;
  scratchbird::engine::internal_api::TypedRelationalDag relational_dag;
  TypedPhysicalNodeDag physical_dag;
  std::size_t maximum_scanned_row_versions = 0;
  std::size_t maximum_decoded_bytes = 0;
  std::size_t maximum_output_rows = 0;
  std::size_t maximum_output_columns = 0;
  std::size_t maximum_output_cells = 0;
  std::function<bool()> cancellation_requested;
  CanonicalExecutionMgaAuthority mga_authority;
  std::optional<CanonicalHeapTableSampleProfile> table_sample_profile;
};

struct HeapPhysicalRegistrationBuildResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::optional<CanonicalPhysicalExecutorRegistration> registration;
  std::shared_ptr<HeapPhysicalDispatchObservation> observation;
};

bool SameHeapPhysicalNodeIdentity(const PhysicalNodeRecord& left,
                                  const PhysicalNodeRecord& right) {
  return left.physical_node_id == right.physical_node_id &&
         left.relational_node_id == right.relational_node_id &&
         left.node_kind == right.node_kind &&
         left.implementation_id == right.implementation_id &&
         left.input_physical_node_ids == right.input_physical_node_ids &&
         left.output_descriptor_ids == right.output_descriptor_ids &&
         left.causal_counter_id == right.causal_counter_id &&
         left.selected_alternative_uuid == right.selected_alternative_uuid &&
         left.executor_capability_uuid == right.executor_capability_uuid &&
         left.executor_capability_abi_version ==
             right.executor_capability_abi_version &&
         left.engine_capability_validated ==
             right.engine_capability_validated;
}

bool SameHeapPhysicalDagAuthority(const TypedPhysicalNodeDag& left,
                                  const TypedPhysicalNodeDag& right) {
  return left.abi_version == right.abi_version &&
         left.selected_plan_uuid == right.selected_plan_uuid &&
         left.root_physical_node_id == right.root_physical_node_id &&
         left.local_transaction_id == right.local_transaction_id &&
         left.statement_snapshot_id == right.statement_snapshot_id &&
         PhysicalMgaStatementContextEqual(left.mga_statement_context,
                                          right.mga_statement_context) &&
         left.bound_sblr_tree_uuid == right.bound_sblr_tree_uuid &&
         left.catalog_epoch_uuid == right.catalog_epoch_uuid &&
         left.security_context_uuid == right.security_context_uuid &&
         left.capability_snapshot_uuid == right.capability_snapshot_uuid &&
         left.resource_snapshot_uuid == right.resource_snapshot_uuid &&
         left.statistics_snapshot_uuid == right.statistics_snapshot_uuid &&
         left.route_snapshot_uuid == right.route_snapshot_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.security_epoch == right.security_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.statistics_generation == right.statistics_generation &&
         left.route_epoch == right.route_epoch &&
         left.route_generation == right.route_generation &&
         left.memory_budget_bytes == right.memory_budget_bytes &&
         left.optimizer_published == right.optimizer_published &&
         left.immutable_node_identity_validated ==
             right.immutable_node_identity_validated &&
         left.capability_validated_before_access ==
             right.capability_validated_before_access &&
         left.data_access_observed == right.data_access_observed &&
         left.nodes.size() == right.nodes.size();
}

std::uint64_t HeapPhysicalResultHandle(const TypedPhysicalNodeDag& dag,
                                       const PhysicalNodeRecord& node) {
  std::uint64_t value = 1469598103934665603ULL;
  const auto mix_byte = [&](const std::uint8_t byte) {
    value ^= byte;
    value *= 1099511628211ULL;
  };
  for (const unsigned char ch : dag.selected_plan_uuid) { mix_byte(ch); }
  for (const unsigned char ch : node.executor_capability_uuid) { mix_byte(ch); }
  for (std::size_t offset = 0; offset < sizeof(node.physical_node_id);
       ++offset) {
    mix_byte(static_cast<std::uint8_t>(
        node.physical_node_id >> (offset * 8u)));
  }
  return value == 0 ? 1 : value;
}

std::string_view HeapTableSampleImplementationId(
    const CanonicalHeapTableSampleProfile& profile) {
  return profile.method == CanonicalHeapTableSampleMethod::kBernoulli
             ? "scan.heap.tablesample.bernoulli.v1"
             : "scan.heap.tablesample.system.v1";
}

std::string_view HeapTableSampleSemanticId(
    const CanonicalHeapTableSampleProfile& profile) {
  return profile.method == CanonicalHeapTableSampleMethod::kBernoulli
             ? "relation.source.tablesample.bernoulli.v1"
             : "relation.source.tablesample.system.v1";
}

scratchbird::engine::internal_api::CanonicalSeededSampleRequest
HeapSeededSampleRequest(const CanonicalHeapTableSampleProfile& profile,
                        const std::size_t input_row_count,
                        const std::size_t maximum_input_row_count) {
  namespace api = scratchbird::engine::internal_api;
  api::CanonicalSeededSampleRequest request;
  request.input_row_count = input_row_count;
  request.method =
      profile.method == CanonicalHeapTableSampleMethod::kBernoulli
          ? api::CanonicalSeededSampleMethod::kBernoulli
          : api::CanonicalSeededSampleMethod::kSystem;
  request.sample_basis_points = profile.sample_basis_points;
  request.repeatable_seed = profile.repeatable_seed;
  request.repeatable_seed_is_bound = profile.repeatable_seed_is_bound;
  request.system_block_row_count = profile.system_block_row_count;
  request.maximum_input_row_count = maximum_input_row_count;
  return request;
}

DescriptorRuntimeDiagnostic ValidateHeapTableSampleProfile(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  if (!request.table_sample_profile.has_value()) return {};
  const auto& profile = *request.table_sample_profile;
  if (profile.method != CanonicalHeapTableSampleMethod::kBernoulli &&
      profile.method != CanonicalHeapTableSampleMethod::kSystem) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE method is outside the accepted profile");
  }
  if (request.relational_dag == nullptr ||
      request.relational_dag->nodes.size() != 1 ||
      request.physical_dag.nodes.size() != 1) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE requires one optimizer-selected heap scan");
  }
  if (profile.predicate_placement ==
      CanonicalHeapTableSamplePredicatePlacement::kBeforeSample) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-PUSHDOWN-V1",
        "predicate pushdown below TABLESAMPLE changes the sample population");
  }
  if (profile.predicate_placement !=
          CanonicalHeapTableSamplePredicatePlacement::kAbsent &&
      profile.predicate_placement !=
          CanonicalHeapTableSamplePredicatePlacement::kAfterSample) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-PUSHDOWN-V1",
        "TABLESAMPLE predicate placement is outside the accepted profile");
  }
  const auto seeded = HeapSeededSampleRequest(profile, 0, 1);
  const auto descriptor_uuid =
      api::CanonicalSeededSampleDescriptorUuid(seeded);
  const auto& relational_node = request.relational_dag->nodes.front();
  const auto& physical_node = request.physical_dag.nodes.front();
  if (descriptor_uuid.empty() ||
      relational_node.semantic_variant_id !=
          HeapTableSampleSemanticId(profile) ||
      physical_node.implementation_id !=
          HeapTableSampleImplementationId(profile)) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE method, seed, rate, or scan profile is unresolved");
  }
  if (physical_node.selected_alternative_uuid != descriptor_uuid) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-SEED-IDENTITY-V1",
        "TABLESAMPLE seed descriptor is absent from selected-plan identity");
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateHeapPhysicalRegistrationRequest(
    const CanonicalHeapPhysicalDagDispatchRequest& request,
    std::vector<const PhysicalNodeRecord*>* heap_nodes) {
  namespace api = scratchbird::engine::internal_api;
  if (heap_nodes == nullptr || request.context == nullptr ||
      request.relational_dag == nullptr) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-BINDING-V1",
        "engine context and typed relational DAG are required");
  }
  const auto& context = *request.context;
  const auto& relational = *request.relational_dag;
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  if (request.mga_authority.origin !=
      CanonicalMgaAuthorityOrigin::kEngineTransactionInventory) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
        "heap registration requires engine transaction-inventory authority");
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!entry_authority.ok) return entry_authority;
  const auto relational_validation = api::ValidateTypedRelationalDag(relational);
  if (!relational_validation.accepted) {
    const auto& issue = relational_validation.issues.front();
    return HeapAcquisitionRefusal(issue.diagnostic_id, issue.field_id);
  }
  const auto physical_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!physical_validation.accepted) {
    const auto& issue = physical_validation.issues.front();
    return HeapAcquisitionRefusal(issue.diagnostic_id, issue.field_id);
  }
  const auto current_mga =
      ValidateCurrentHeapPhysicalMgaAuthority(context, request.physical_dag);
  if (!current_mga.ok) {
    return current_mga;
  }
  if (relational.wire_version != 2 ||
      request.physical_dag.abi_version != 2 ||
      !request.physical_dag.optimizer_published ||
      !request.physical_dag.immutable_node_identity_validated ||
      !request.physical_dag.capability_validated_before_access ||
      request.physical_dag.data_access_observed) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-SCOPE-V1",
        "wire-v2 optimizer-published physical authority is required");
  }
  if (!request.cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > kMaximumHeapOutputColumns) {
    return HeapAcquisitionRefusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "valid nonzero heap dispatch shape bounds and cancellation probe are "
        "required");
  }
  if (request.cancellation_requested()) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
        "heap physical dispatch cancelled before registration");
  }
  const auto& authorization = context.authorization_context;
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      request.physical_dag.local_transaction_id !=
          context.local_transaction_id ||
      request.physical_dag.statement_snapshot_id !=
          context.snapshot_visible_through_local_transaction_id ||
      !context.statement_metadata_snapshot_engine_owned ||
      !context.security_context_present || !authorization.present ||
      relational.bound_sblr_tree_uuid !=
          request.physical_dag.bound_sblr_tree_uuid ||
      relational.statement_uuid != context.statement_uuid.canonical ||
      relational.owning_transaction_uuid !=
          context.transaction_uuid.canonical ||
      relational.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      relational.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      relational.local_transaction_id != context.local_transaction_id ||
      relational.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id ||
      relational.bound_catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      request.physical_dag.catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      relational.bound_security_context_uuid !=
          authorization.authority_uuid.canonical ||
      request.physical_dag.security_context_uuid !=
          authorization.authority_uuid.canonical ||
      request.physical_dag.catalog_generation !=
          context.catalog_generation_id ||
      context.catalog_generation_id !=
          authorization.catalog_generation_id ||
      request.physical_dag.security_epoch != context.security_epoch ||
      context.security_epoch != authorization.security_epoch ||
      request.physical_dag.policy_epoch != authorization.policy_epoch ||
      request.physical_dag.resource_epoch != context.resource_epoch ||
      request.physical_dag.capability_snapshot_uuid !=
          context.optimizer_capability_snapshot_uuid.canonical ||
      request.physical_dag.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      request.physical_dag.route_snapshot_uuid !=
          context.optimizer_route_snapshot_uuid.canonical ||
      request.physical_dag.route_epoch != context.optimizer_route_epoch ||
      request.physical_dag.route_generation !=
          context.optimizer_route_generation ||
      request.physical_dag.memory_budget_bytes !=
          context.optimizer_memory_budget_bytes ||
      context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      request.maximum_decoded_bytes > context.optimizer_memory_budget_bytes ||
      request.maximum_scanned_row_versions >
          context.optimizer_maximum_candidate_count ||
      request.maximum_output_rows >
          context.optimizer_maximum_candidate_count) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-AUTHORITY-V1",
        "heap registration authority is stale, missing, or over budget");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-SCOPE-V1",
        "prepared or cached descriptor dispatch is outside this profile");
  }
  const auto sample_validation = ValidateHeapTableSampleProfile(request);
  if (!sample_validation.ok) return sample_validation;

  heap_nodes->clear();
  const std::string_view expected_implementation =
      request.table_sample_profile.has_value()
          ? HeapTableSampleImplementationId(*request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  std::string capability_uuid;
  std::uint32_t capability_abi = 0;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.implementation_id != expected_implementation) { continue; }
    if (node.node_kind != PhysicalNodeKind::kScan ||
        !node.input_physical_node_ids.empty() ||
        node.output_descriptor_ids.empty() ||
        node.output_descriptor_ids.size() > request.maximum_output_columns ||
        node.output_descriptor_ids.size() > kMaximumHeapOutputColumns ||
        node.executor_capability_uuid.empty() ||
        node.executor_capability_abi_version == 0 ||
        !node.engine_capability_validated) {
      return HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
          "heap executor capability is incomplete");
    }
    if (heap_nodes->empty()) {
      capability_uuid = node.executor_capability_uuid;
      capability_abi = node.executor_capability_abi_version;
    } else if (node.executor_capability_uuid != capability_uuid ||
               node.executor_capability_abi_version != capability_abi) {
      return HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
          "heap executor capability identity is inconsistent");
    }
    heap_nodes->push_back(&node);
  }
  if (heap_nodes->empty()) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
        "the bound heap scan implementation is not present in the selected "
        "physical DAG");
  }
  return {};
}

HeapPhysicalRegistrationBuildResult BuildHeapPhysicalRegistration(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  HeapPhysicalRegistrationBuildResult result;
  std::vector<const PhysicalNodeRecord*> heap_nodes;
  result.diagnostic =
      ValidateHeapPhysicalRegistrationRequest(request, &heap_nodes);
  if (!result.diagnostic.ok) { return result; }

  auto state = std::make_shared<HeapPhysicalExecutorState>();
  state->context = *request.context;
  state->relational_dag = *request.relational_dag;
  state->physical_dag = request.physical_dag;
  state->maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  state->maximum_decoded_bytes = request.maximum_decoded_bytes;
  state->maximum_output_rows = request.maximum_output_rows;
  state->maximum_output_columns = request.maximum_output_columns;
  state->maximum_output_cells = request.maximum_output_cells;
  state->cancellation_requested = request.cancellation_requested;
  state->mga_authority = request.mga_authority;
  state->table_sample_profile = request.table_sample_profile;
  result.observation = std::make_shared<HeapPhysicalDispatchObservation>();

  CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = heap_nodes.front()->node_kind;
  registration.implementation_id = heap_nodes.front()->implementation_id;
  registration.executor_capability_uuid =
      heap_nodes.front()->executor_capability_uuid;
  registration.executor_capability_abi_version =
      heap_nodes.front()->executor_capability_abi_version;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [state, observation = result.observation](
          const TypedPhysicalNodeDag& dispatched_dag,
          const PhysicalNodeRecord& dispatched_node,
          const std::vector<CanonicalPhysicalDispatchInput>& inputs) {
        CanonicalPhysicalDispatchStepResult step;
        observation->callback_invoked = true;
        // QOW-SOURCE-QRY-004-DATA-ACCESS-OBSERVATION-V1
        // This engine-owned callback always knows whether the bounded heap
        // acquisition crossed its physical-read boundary, including refusal
        // returns.
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        if (!inputs.empty()) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-INPUT-V1",
              "leaf heap executor does not accept input batches");
          return step;
        }
        const auto expected_node = std::ranges::find_if(
            state->physical_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id ==
                     dispatched_node.physical_node_id;
            });
        if (!SameHeapPhysicalDagAuthority(dispatched_dag,
                                          state->physical_dag) ||
            expected_node == state->physical_dag.nodes.end() ||
            !SameHeapPhysicalNodeIdentity(dispatched_node, *expected_node) ||
            dispatched_node.node_kind != PhysicalNodeKind::kScan ||
            dispatched_node.implementation_id !=
                (state->table_sample_profile.has_value()
                     ? HeapTableSampleImplementationId(
                           *state->table_sample_profile)
                     : std::string_view{"scan.heap.v1"}) ||
            !dispatched_node.input_physical_node_ids.empty()) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
              "dispatched heap node differs from captured optimizer authority");
          return step;
        }
        const auto callback_entry = RevalidateCanonicalExecutionMgaAuthority(
            state->mga_authority, dispatched_dag);
        if (!callback_entry.ok) {
          step.diagnostic = callback_entry;
          return step;
        }

        auto acquisition_relational_dag = state->relational_dag;
        auto acquisition_physical_dag = state->physical_dag;
        std::erase_if(acquisition_relational_dag.nodes,
                      [&](const auto& candidate) {
                        return candidate.node_id !=
                               dispatched_node.relational_node_id;
                      });
        if (acquisition_relational_dag.nodes.size() != 1) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
              "heap acquisition leaf is unresolved in the relational DAG");
          return step;
        }
        acquisition_relational_dag.root_node_id =
            dispatched_node.relational_node_id;
        const auto& acquisition_node =
            acquisition_relational_dag.nodes.front();
        const std::unordered_set<std::uint32_t> acquisition_expressions(
            acquisition_node.bound_expression_ids.begin(),
            acquisition_node.bound_expression_ids.end());
        const std::unordered_set<std::uint32_t> acquisition_descriptors(
            acquisition_node.output_descriptor_ids.begin(),
            acquisition_node.output_descriptor_ids.end());
        std::erase_if(acquisition_relational_dag.expressions,
                      [&](const auto& expression) {
                        return !acquisition_expressions.contains(
                            expression.expression_id);
                      });
        std::erase_if(acquisition_relational_dag.descriptors,
                      [&](const auto& descriptor) {
                        return !acquisition_descriptors.contains(
                            descriptor.descriptor_id);
                      });
        std::erase_if(acquisition_relational_dag.outputs,
                      [&](const auto& output) {
                        return output.relation_node_id !=
                               acquisition_node.node_id;
                      });
        acquisition_relational_dag.values_rows.clear();
        acquisition_relational_dag.grouping_sets.clear();
        acquisition_relational_dag.properties.clear();
        std::erase_if(acquisition_physical_dag.nodes,
                      [&](const auto& candidate) {
                        return candidate.physical_node_id !=
                               dispatched_node.physical_node_id;
                      });
        acquisition_physical_dag.root_physical_node_id =
            dispatched_node.physical_node_id;
        if (state->table_sample_profile.has_value()) {
          // The sample executor owns the selected sample node. Its private
          // acquisition child is a plain heap scan that returns only the
          // statement-MGA-visible population; no public sample-shaped direct
          // acquisition route can return an unsampled rowset.
          acquisition_relational_dag.nodes.front().semantic_variant_id =
              "relation.source.v1";
          acquisition_physical_dag.nodes.front().implementation_id =
              "scan.heap.v1";
        }
        CanonicalHeapRelationAcquisitionRequest acquisition_request;
        acquisition_request.context = &state->context;
        acquisition_request.relational_dag = &acquisition_relational_dag;
        acquisition_request.physical_dag = acquisition_physical_dag;
        acquisition_request.selected_physical_node_id =
            dispatched_node.physical_node_id;
        acquisition_request.maximum_scanned_row_versions =
            state->maximum_scanned_row_versions;
        acquisition_request.maximum_decoded_bytes =
            state->maximum_decoded_bytes;
        acquisition_request.maximum_output_rows =
            state->table_sample_profile.has_value()
                ? state->maximum_scanned_row_versions
                : state->maximum_output_rows;
        acquisition_request.maximum_output_columns =
            state->maximum_output_columns;
        acquisition_request.maximum_output_cells = state->maximum_output_cells;
        if (state->table_sample_profile.has_value() &&
            !CheckedHeapCellCount(
                state->maximum_scanned_row_versions,
                dispatched_node.output_descriptor_ids.size(),
                &acquisition_request.maximum_output_cells)) {
          step.diagnostic = HeapAcquisitionRefusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "TABLESAMPLE visible-population cell bound overflows size_t");
          return step;
        }
        acquisition_request.cancellation_requested =
            state->cancellation_requested;
        acquisition_request.mga_authority = state->mga_authority;
        auto acquisition =
            ExecuteCanonicalHeapRelationAcquisition(acquisition_request);
        observation->data_access_observed = acquisition.data_access_observed;
        observation->cancellation_observed = acquisition.cancellation_observed;
        step.data_access_observed = acquisition.data_access_observed;
        if (!acquisition.diagnostic.ok) {
          step.diagnostic = std::move(acquisition.diagnostic);
          return step;
        }
        if (!PhysicalMgaStatementContextEqual(
                acquisition.mga_statement_context,
                state->mga_authority.statement_context)) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
              "heap acquisition returned a different MGA statement context");
          return step;
        }
        const auto callback_result = RevalidateCanonicalExecutionMgaAuthority(
            state->mga_authority, dispatched_dag);
        if (!callback_result.ok) {
          step.diagnostic = callback_result;
          return step;
        }

        std::optional<CanonicalHeapTableSampleActuals> sample_actuals;
        if (state->table_sample_profile.has_value()) {
          namespace api = scratchbird::engine::internal_api;
          const auto visible_input_row_count =
              acquisition.output_batch.rows.size();
          if (acquisition.emitted_record_uuids.size() !=
                  visible_input_row_count ||
              acquisition.emitted_row_version_uuids.size() !=
                  visible_input_row_count) {
            step.diagnostic = HeapAcquisitionRefusal(
                "QOW-DIAG-QRY-015-VISIBILITY-CARRIER-V1",
                "TABLESAMPLE input lost visible record/version identity");
            return step;
          }
          auto sample_request = HeapSeededSampleRequest(
              *state->table_sample_profile, visible_input_row_count,
              state->maximum_scanned_row_versions);
          const auto sampled = api::ExecuteCanonicalSeededSample(
              sample_request);
          if (!sampled.accepted) {
            step.diagnostic = HeapAcquisitionRefusal(
                sampled.diagnostic_code, sampled.detail);
            return step;
          }
          std::size_t sampled_output_cell_count = 0;
          if (sampled.selected_row_indices.size() >
                  state->maximum_output_rows ||
              !CheckedHeapCellCount(
                  sampled.selected_row_indices.size(),
                  acquisition.output_batch.columns.size(),
                  &sampled_output_cell_count) ||
              sampled_output_cell_count > state->maximum_output_cells) {
            step.diagnostic = HeapAcquisitionRefusal(
                "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                "TABLESAMPLE output exceeds the admitted result bound");
            return step;
          }
          decltype(acquisition.output_batch.rows) rows;
          std::vector<std::string> record_uuids;
          std::vector<std::string> version_uuids;
          rows.reserve(sampled.selected_row_indices.size());
          record_uuids.reserve(sampled.selected_row_indices.size());
          version_uuids.reserve(sampled.selected_row_indices.size());
          for (const auto row : sampled.selected_row_indices) {
            if (row >= visible_input_row_count) {
              step.diagnostic = HeapAcquisitionRefusal(
                  "QOW-DIAG-QRY-015-SAMPLE-INDEX-V1",
                  "TABLESAMPLE selected an out-of-range visible row");
              return step;
            }
            rows.push_back(acquisition.output_batch.rows[row]);
            record_uuids.push_back(acquisition.emitted_record_uuids[row]);
            version_uuids.push_back(
                acquisition.emitted_row_version_uuids[row]);
          }
          acquisition.output_batch.rows = std::move(rows);
          acquisition.emitted_record_uuids = std::move(record_uuids);
          acquisition.emitted_row_version_uuids = std::move(version_uuids);
          acquisition.counters.emitted_row_count =
              acquisition.output_batch.rows.size();
          acquisition.counters.materialized_cell_count =
              sampled_output_cell_count;
          CanonicalHeapTableSampleActuals actuals;
          actuals.sample_descriptor_uuid =
              api::CanonicalSeededSampleDescriptorUuid(sample_request);
          actuals.method_id = sampled.method_id;
          actuals.sample_basis_points = sample_request.sample_basis_points;
          actuals.visible_input_row_count = visible_input_row_count;
          actuals.examined_unit_count = sampled.examined_unit_count;
          actuals.sampled_output_row_count =
              sampled.selected_row_indices.size();
          actuals.repeatable_seed_bound =
              sample_request.repeatable_seed_is_bound;
          actuals.sampling_applied_after_mga_visibility = true;
          actuals.predicate_pushdown_legality_validated = true;
          sample_actuals = std::move(actuals);
        }

        step.diagnostic = {};
        step.selected_plan_uuid = acquisition.selected_plan_uuid;
        step.executed_physical_node_id =
            acquisition.executed_physical_node_id;
        step.causal_counter_id = acquisition.causal_counter_id;
        step.result_handle_id =
            HeapPhysicalResultHandle(dispatched_dag, dispatched_node);
        step.output_descriptor_ids = dispatched_node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound =
            acquisition.authority.engine_mga_snapshot_bound;
        step.output_row_count = acquisition.counters.emitted_row_count;
        step.rows_examined = acquisition.counters.scanned_row_version_count;
        if (sample_actuals.has_value()) {
          step.table_sample_actuals = std::move(sample_actuals);
        }
        step.heap_read_counters = acquisition.counters;
        step.heap_read_authority = acquisition.authority;
        step.current_relation_descriptor_uuid =
            acquisition.current_relation_descriptor_uuid;
        step.current_relation_descriptor_generation =
            acquisition.current_relation_descriptor_generation;
        step.materialized_output_batch = std::move(acquisition.output_batch);
        step.mga_statement_context = state->mga_authority.statement_context;
        return step;
      };
  result.registration = std::move(registration);
  return result;
}

CanonicalPhysicalDagDispatchResult HeapPhysicalDispatchRefusal(
    DescriptorRuntimeDiagnostic diagnostic,
    const bool execution_started = false,
    const bool data_access_observed = false) {
  CanonicalPhysicalDagDispatchResult result;
  result.diagnostic = std::move(diagnostic);
  result.execution_started = execution_started;
  result.data_access_observed = data_access_observed;
  return result;
}

}  // namespace

CanonicalHeapPhysicalRegistrationResult
BuildCanonicalHeapPhysicalRegistration(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  CanonicalHeapPhysicalRegistrationResult result;
  if (request.context == nullptr) {
    result.diagnostic = HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
        "engine context is required");
    return result;
  }
  auto owned = request;
  owned.mga_authority = BuildCurrentHeapExecutionMgaAuthority(
      *request.context, request.physical_dag);
  auto built = BuildHeapPhysicalRegistration(owned);
  result.diagnostic = std::move(built.diagnostic);
  result.registration = std::move(built.registration);
  result.mga_authority = std::move(owned.mga_authority);
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-DISPATCH-V1
CanonicalPhysicalDagDispatchResult ExecuteCanonicalHeapPhysicalDagDispatch(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  if (request.context == nullptr) {
    return HeapPhysicalDispatchRefusal(HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-BINDING-V1",
        "engine context is required"));
  }
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(request.context->database_path);
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!entry_authority.ok) {
    return HeapPhysicalDispatchRefusal(entry_authority);
  }
  auto built = BuildHeapPhysicalRegistration(request);
  if (!built.diagnostic.ok || !built.registration.has_value() ||
      built.observation == nullptr) {
    if (built.diagnostic.ok) {
      built.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
          "engine heap executor registration is unavailable");
    }
    return HeapPhysicalDispatchRefusal(std::move(built.diagnostic));
  }
  const auto& physical = request.physical_dag;
  const std::string_view expected_implementation =
      request.table_sample_profile.has_value()
          ? HeapTableSampleImplementationId(*request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  if (physical.nodes.size() != 1 ||
      physical.root_physical_node_id !=
          physical.nodes.front().physical_node_id ||
      physical.nodes.front().node_kind != PhysicalNodeKind::kScan ||
      physical.nodes.front().implementation_id != expected_implementation ||
      !physical.nodes.front().input_physical_node_ids.empty()) {
    return HeapPhysicalDispatchRefusal(HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-ROOT-V1",
        "exactly one input-free bound heap scan root is required"));
  }

  CanonicalPhysicalDagDispatchRequest dispatch_request;
  dispatch_request.physical_dag = physical;
  dispatch_request.mga_authority = request.mga_authority;
  dispatch_request.available_executors.push_back(
      std::move(*built.registration));
  auto dispatched = ExecuteCanonicalPhysicalDag(dispatch_request);
  dispatched.execution_started = built.observation->callback_invoked;
  dispatched.data_access_observed =
      built.observation->data_access_observed;
  if (!dispatched.diagnostic.ok) {
    if (!dispatched.executed_steps.empty() ||
        dispatched.root_result_handle_id != 0 ||
        !dispatched.root_output_descriptor_ids.empty()) {
      return HeapPhysicalDispatchRefusal(
          HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-ATOMICITY-V1",
              "failed heap dispatch exposed a partial result"),
          built.observation->callback_invoked,
          built.observation->data_access_observed);
    }
    return dispatched;
  }
  if (!built.observation->callback_invoked ||
      dispatched.executed_steps.size() != 1 ||
      dispatched.root_result_handle_id == 0 ||
      dispatched.root_output_descriptor_ids !=
          physical.nodes.front().output_descriptor_ids ||
      dispatched.executed_root_physical_node_id !=
          physical.nodes.front().physical_node_id ||
      dispatched.root_causal_counter_id !=
          physical.nodes.front().causal_counter_id ||
      !dispatched.executed_steps.front().materialized_output_batch.has_value() ||
      !dispatched.executed_steps.front().heap_read_counters.has_value() ||
      !dispatched.executed_steps.front().heap_read_authority.has_value() ||
      !PhysicalMgaStatementContextEqual(
          dispatched.mga_statement_context,
          request.mga_authority.statement_context) ||
      !PhysicalMgaStatementContextEqual(
          dispatched.executed_steps.front().mga_statement_context,
          request.mga_authority.statement_context)) {
    return HeapPhysicalDispatchRefusal(
        HeapAcquisitionRefusal(
            "QOW-DIAG-QRY-004-HEAP-DISPATCH-EVIDENCE-V1",
            "heap dispatch lost materialized or causal evidence"),
        built.observation->callback_invoked,
        built.observation->data_access_observed);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return HeapPhysicalDispatchRefusal(
        result_authority, true, built.observation->data_access_observed);
  }
  return dispatched;
}

}  // namespace scratchbird::engine::executor

namespace scratchbird::engine::internal_api {

// QOW-SOURCE-QRY-004-HEAP-RESULT-V1
// Closes the one-leaf heap profile by deriving all executor and publication
// bindings from admitted engine-owned authority and then entering the single
// canonical optimizer-selected execution spine.
CanonicalOptimizerSelectedExecutionResult
ExecuteCanonicalHeapOptimizerSelectedDag(
    const CanonicalHeapOptimizerSelectedExecutionRequest& request) {
  namespace exec = scratchbird::engine::executor;
  const auto refuse = [](std::string diagnostic_id,
                         std::uint64_t physical_node_id,
                         std::string field_id) {
    CanonicalOptimizerSelectedExecutionResult result;
    result.issues.push_back({std::move(diagnostic_id), physical_node_id,
                             std::move(field_id)});
    return result;
  };

  const auto& context = request.context;
  const auto& relational = request.relational_dag;
  const auto& physical = request.selected_physical_dag;
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  if (!exec::IsCanonicalHeapBindingUuid(context.statement_uuid.canonical) ||
      !exec::IsCanonicalHeapBindingUuid(request.execution_attempt_uuid) ||
      !exec::IsCanonicalHeapBindingUuid(
          request.transaction_effect_evidence_uuid) ||
      context.statement_uuid.canonical == request.execution_attempt_uuid ||
      context.statement_uuid.canonical ==
          request.transaction_effect_evidence_uuid ||
      request.execution_attempt_uuid ==
          request.transaction_effect_evidence_uuid) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-IDENTITY-V1", 0,
                  "statement_execution_effect_uuid");
  }
  if (!request.cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > exec::kMaximumHeapOutputColumns) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0,
                  "heap_result_resource_bounds");
  }

  const auto relational_validation = ValidateTypedRelationalDag(relational);
  if (!relational_validation.accepted) {
    const auto& issue = relational_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.node_id, issue.field_id);
  }
  const auto physical_validation = exec::ValidateTypedPhysicalNodeDag(physical);
  if (!physical_validation.accepted) {
    const auto& issue = physical_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.physical_node_id, issue.field_id);
  }
  const auto current_mga =
      exec::ValidateCurrentHeapPhysicalMgaAuthority(context, physical);
  if (!current_mga.ok) {
    return refuse(current_mga.diagnostic_code, 0, current_mga.detail);
  }
  auto mga_authority =
      exec::BuildCurrentHeapExecutionMgaAuthority(context, physical);
  const auto carrier_validation =
      exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority, physical);
  if (!carrier_validation.ok) {
    return refuse(carrier_validation.diagnostic_code, 0,
                  carrier_validation.detail);
  }
  const std::string_view expected_relational_semantic =
      request.table_sample_profile.has_value()
          ? exec::HeapTableSampleSemanticId(*request.table_sample_profile)
          : std::string_view{"relation.source.v1"};
  const std::string_view expected_physical_implementation =
      request.table_sample_profile.has_value()
          ? exec::HeapTableSampleImplementationId(
                *request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  if (relational.wire_version != 2 || relational.nodes.size() != 1 ||
      relational.root_node_id != relational.nodes.front().node_id ||
      relational.nodes.front().node_kind != RelationalDagNodeKind::kScan ||
      relational.nodes.front().semantic_variant_id !=
          expected_relational_semantic ||
      !relational.nodes.front().input_node_ids.empty() ||
      relational.nodes.front().required_object_uuids.size() != 1 ||
      relational.nodes.front().bound_expression_ids.empty() ||
      relational.nodes.front().bound_expression_ids.size() !=
          relational.nodes.front().output_descriptor_ids.size() ||
      relational.nodes.front().output_descriptor_ids.size() !=
          relational.expressions.size() ||
      relational.expressions.size() != relational.descriptors.size() ||
      relational.descriptors.size() != relational.outputs.size() ||
      relational.outputs.size() > request.maximum_output_columns ||
      relational.outputs.size() > exec::kMaximumHeapOutputColumns) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-PROFILE-V1", 0,
                  "bound_relation_source_one_leaf_complete_width");
  }
  if (physical.abi_version != 2 || physical.nodes.size() != 1 ||
      physical.root_physical_node_id != physical.nodes.front().physical_node_id ||
      physical.nodes.front().node_kind != exec::PhysicalNodeKind::kScan ||
      physical.nodes.front().implementation_id !=
          expected_physical_implementation ||
      !physical.nodes.front().input_physical_node_ids.empty() ||
      physical.nodes.front().relational_node_id != relational.root_node_id ||
      physical.nodes.front().output_descriptor_ids !=
          relational.nodes.front().output_descriptor_ids) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-PROFILE-V1", 0,
                  "scan.heap.v1_one_leaf_root");
  }

  const auto& relational_node = relational.nodes.front();
  const auto& physical_node = physical.nodes.front();
  if (!exec::IsCanonicalHeapBindingUuid(
          relational_node.required_object_uuids.front())) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                  physical_node.physical_node_id,
                  "relation_uuid");
  }

  std::vector<const RelationalOutputRecord*> ordered_outputs;
  ordered_outputs.reserve(relational.outputs.size());
  for (const auto& output : relational.outputs) {
    if (output.relation_node_id == relational_node.node_id) {
      ordered_outputs.push_back(&output);
    }
  }
  if (ordered_outputs.size() != relational.outputs.size()) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                  physical_node.physical_node_id,
                  "relation_output_coverage");
  }
  std::vector<const RelationalTypeDescriptor*> ordered_descriptors;
  ordered_descriptors.reserve(ordered_outputs.size());
  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> expression_ids;
  std::unordered_set<std::uint32_t> descriptor_ids;
  std::unordered_set<std::string> bound_column_uuids;
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    const auto expression = relational.expressions.begin() + ordinal;
    const auto descriptor = relational.descriptors.begin() + ordinal;
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        !output_ids.insert(output.output_id).second ||
        output.output_name_utf8.empty() ||
        output.descriptor_id != relational_node.output_descriptor_ids[ordinal] ||
        expression->expression_id != output.expression_id ||
        !expression_ids.insert(expression->expression_id).second ||
        relational_node.bound_expression_ids[ordinal] !=
            expression->expression_id ||
        expression->expression_kind != RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        expression->result_descriptor_id != output.descriptor_id ||
        !expression->bound_name_uuid.has_value() ||
        !exec::IsCanonicalHeapBindingUuid(*expression->bound_name_uuid) ||
        !bound_column_uuids.insert(*expression->bound_name_uuid).second ||
        descriptor->descriptor_id != output.descriptor_id ||
        !descriptor_ids.insert(descriptor->descriptor_id).second ||
        !exec::IsCanonicalHeapBindingUuid(descriptor->descriptor_uuid) ||
        !exec::IsCanonicalHeapBindingUuid(descriptor->type_uuid) ||
        descriptor->nullability == RelationalNullability::kUnknown ||
        (descriptor->collation_uuid.has_value() &&
         !exec::IsCanonicalHeapBindingUuid(*descriptor->collation_uuid)) ||
        (descriptor->timezone_profile_id.has_value() &&
         descriptor->timezone_profile_id->empty())) {
      return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                    physical_node.physical_node_id,
                    "relational_expression_descriptor_physical_agreement");
    }
    ordered_descriptors.push_back(&*descriptor);
  }

  exec::CanonicalHeapPhysicalDagDispatchRequest registration_request;
  registration_request.context = &request.context;
  registration_request.relational_dag = &request.relational_dag;
  registration_request.physical_dag = request.selected_physical_dag;
  registration_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  registration_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  registration_request.maximum_output_rows = request.maximum_output_rows;
  registration_request.maximum_output_columns = request.maximum_output_columns;
  registration_request.maximum_output_cells = request.maximum_output_cells;
  registration_request.cancellation_requested = request.cancellation_requested;
  registration_request.mga_authority = mga_authority;
  registration_request.table_sample_profile = request.table_sample_profile;
  auto built = exec::BuildHeapPhysicalRegistration(registration_request);
  if (!built.diagnostic.ok || !built.registration.has_value() ||
      built.observation == nullptr) {
    if (built.diagnostic.ok) {
      built.diagnostic = exec::HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
          "engine heap executor registration is unavailable");
    }
    return refuse(std::move(built.diagnostic.diagnostic_code),
                  physical_node.physical_node_id,
                  std::move(built.diagnostic.detail));
  }

  CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = request.selected_physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      request.selected_physical_dag.statistics_snapshot_uuid;
  selected.mga_authority = mga_authority;
  selected.available_executors.push_back(std::move(*built.registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      context.statement_uuid.canonical;
  selected.result_publication_request.execution_attempt_uuid =
      request.execution_attempt_uuid;
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.column_bindings.reserve(
      ordered_outputs.size());
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    const auto& descriptor = *ordered_descriptors[ordinal];
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = ordinal;
    published.name_utf8 = output.output_name_utf8;
    published.descriptor_uuid = descriptor.descriptor_uuid;
    published.type_uuid = descriptor.type_uuid;
    published.nullability =
        descriptor.nullability == RelationalNullability::kNullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull;
    published.collation_uuid = descriptor.collation_uuid;
    published.timezone_profile_id = descriptor.timezone_profile_id;
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  selected.result_publication_request.transaction_effect_evidence_uuid =
      request.transaction_effect_evidence_uuid;
  selected.result_publication_request.maximum_row_count =
      request.maximum_output_rows;
  return ExecuteCanonicalOptimizerSelectedDag(selected);
}

}  // namespace scratchbird::engine::internal_api
