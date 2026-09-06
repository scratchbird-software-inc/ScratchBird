// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_event_sequence_allocator.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_EVENT_SEQUENCE_ALLOCATOR_IMPLEMENTATION_AUTHORITY
constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr const char* kEventSequenceAllocatorMagic = "SBMGAEVSEQ1";
constexpr std::string_view kSealedTableMetadataKindV2 =
    "TABLE_METADATA_SEALED_DESCRIPTOR_V2";

std::string RowStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_row_versions";
}

std::string MetadataStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_relation_metadata";
}

std::string IndexStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_index_entries";
}

std::string EventSequenceAllocatorStorePath(
    const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_event_sequence_allocator";
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

std::string JoinLine(const std::vector<std::string>& fields) {
  std::string line;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) { line.push_back('\t'); }
    line += fields[index];
  }
  return line;
}

std::uint64_t ParseU64(const std::string& text,
                       std::uint64_t fallback = 0) {
  if (text.empty()) { return fallback; }
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) { return lines; }
  std::error_code ignored;
  const auto bytes = std::filesystem::file_size(path, ignored);
  if (!ignored && bytes != static_cast<std::uintmax_t>(-1)) {
    lines.reserve(
        static_cast<std::size_t>(std::max<std::uintmax_t>(1, bytes / 128)));
  }
  std::string line;
  while (std::getline(in, line)) { lines.push_back(line); }
  return lines;
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
  for (const auto& line : lines) { buffer_bytes += line.size() + 1; }
  std::string buffer;
  buffer.reserve(buffer_bytes);
  for (const auto& line : lines) {
    buffer.append(line);
    buffer.push_back('\n');
  }
  out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  out.flush();
  if (stream_flushes != nullptr) { ++(*stream_flushes); }
  return static_cast<bool>(out);
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
    if (fields.size() < 7 || fields[0] != kEventSequenceAllocatorMagic ||
        fields[1] != "RANGE" || fields[2] != stream_kind ||
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
  const auto durable =
      LoadDurableEventSequenceState(context, stream_kind, stream_path);
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
  reservation.diagnostic =
      MakeInvalidRequestDiagnostic("mga.event_sequence_allocator", reason);
  return reservation;
}

}  // namespace

MgaEventSequenceRangeReservation ReserveEventSequenceRange(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t count,
    const std::function<std::uint64_t()>& bootstrap_loader,
    std::vector<std::string>* deferred_allocator_lines) {
  if (context.database_path.empty()) {
    return RefuseEventSequenceReservation(
        context, stream_kind, stream_path, "database_path_required");
  }
  if (stream_kind.empty() || stream_path.empty()) {
    return RefuseEventSequenceReservation(
        context, stream_kind, stream_path, "stream_identity_required");
  }
  const std::uint64_t normalized_count = count == 0 ? 1 : count;
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  const std::string cache_key = EventSequenceStreamKey(stream_kind, stream_path);
  const auto cache_it = EventSequenceCache().find(cache_key);
  std::uint64_t next_sequence = 0;
  bool bootstrapped = false;
  std::string route = "process_cache_after_durable_reservation";
  if (cache_it != EventSequenceCache().end() && cache_it->second != 0) {
    next_sequence = cache_it->second;
  } else {
    const auto durable =
        LoadDurableEventSequenceState(context, stream_kind, stream_path);
    if (durable.found) {
      next_sequence = durable.next;
      route = "durable_allocator_state";
    } else {
      next_sequence = bootstrap_loader();
      bootstrapped = true;
      route = "bootstrap_store_scan";
    }
  }
  if (next_sequence == 0) { next_sequence = 1; }
  if (normalized_count >
      std::numeric_limits<std::uint64_t>::max() - next_sequence) {
    return RefuseEventSequenceReservation(
        context, stream_kind, stream_path, "event_sequence_range_overflow");
  }
  const std::uint64_t first = next_sequence;
  const std::uint64_t next = next_sequence + normalized_count;
  const std::string allocator_path = EventSequenceAllocatorStorePath(context);
  const std::string line = JoinLine(
      {kEventSequenceAllocatorMagic,
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
    return RefuseEventSequenceReservation(
        context, stream_kind, stream_path, "durable_allocator_append_failed");
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

void AbandonDeferredEventSequenceReservation(
    const MgaEventSequenceRangeReservation& reservation) {
  if (!reservation.ok || reservation.stream_kind.empty() ||
      reservation.stream_path.empty()) {
    return;
  }
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  const std::string key = EventSequenceStreamKey(
      reservation.stream_kind, reservation.stream_path);
  const auto found = EventSequenceCache().find(key);
  if (found != EventSequenceCache().end() &&
      found->second == reservation.next) {
    found->second = reservation.first;
  }
}

bool AppendDeferredEventSequenceAllocatorLines(
    const EngineRequestContext& context,
    std::vector<std::string>* lines,
    MgaRelationHotAppendCounters* counters) {
  if (lines == nullptr || lines->empty()) { return true; }
  const bool ok = AppendLines(
      EventSequenceAllocatorStorePath(context),
      *lines,
      counters == nullptr ? nullptr : &counters->allocator_stream_opens,
      counters == nullptr ? nullptr : &counters->allocator_stream_flushes);
  if (ok && counters != nullptr) {
    counters->allocator_range_records_appended +=
        static_cast<std::uint64_t>(lines->size());
  }
  if (ok) { lines->clear(); }
  return ok;
}

std::uint64_t ScanNextRowEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(RowStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        fields[1] == "ROW_VERSION") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextRowEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(
      context, "row_versions", RowStorePath(context),
      ScanNextRowEventSequence(context));
}

std::uint64_t ScanNextIndexEventSequence(const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(IndexStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        fields[1] == "INDEX_ENTRY") {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextIndexEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(
      context, "index_entries", IndexStorePath(context),
      ScanNextIndexEventSequence(context));
}

std::uint64_t ScanNextMetadataEventSequence(
    const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  for (const auto& line : ReadLines(MetadataStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        (fields[1] == "TABLE_METADATA" ||
         fields[1] == kSealedTableMetadataKindV2 ||
         fields[1] == "INDEX_METADATA" ||
         fields[1] == "CONSTRAINT_MUTATION_BATCH" ||
         fields[1] == "BIGINT_IDENTITY_MIGRATION_BATCH" ||
         fields[1] == "INT32_IDENTITY_MIGRATION_BATCH" ||
         fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH")) {
      max_sequence = std::max(max_sequence, ParseU64(fields[3]));
    }
  }
  return max_sequence + 1;
}

std::uint64_t NextMetadataEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(
      context, "relation_metadata", MetadataStorePath(context),
      ScanNextMetadataEventSequence(context));
}

void ClearMgaEventSequenceRangeCacheForTesting() {
  const std::lock_guard<std::mutex> guard(EventSequenceCacheMutex());
  EventSequenceCache().clear();
}

MgaEventSequenceRangeReservation ReserveMgaRowEventSequenceRangeForTesting(
    const EngineRequestContext& context,
    std::uint64_t count) {
  return ReserveEventSequenceRange(
      context, "row_versions", RowStorePath(context), count,
      [&context]() { return ScanNextRowEventSequence(context); });
}

}  // namespace scratchbird::engine::internal_api
