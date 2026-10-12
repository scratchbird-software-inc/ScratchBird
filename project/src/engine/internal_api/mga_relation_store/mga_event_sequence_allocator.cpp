// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_locator.hpp"
#include "mga_relation_store/mga_event_sequence_allocator.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/name_registry_codec.hpp"

#include <algorithm>
#include <charconv>
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

std::string OwnedSequenceStreamPath(const EngineRequestContext& context,
                                   std::string_view kind) {
  if (kind == "row_versions") return RowStorePath(context);
  if (kind == "relation_metadata") return MetadataStorePath(context);
  if (kind == "index_entries") return IndexStorePath(context);
  if (kind == "secondary_index_delta_ledger")
    return context.database_path + ".sb.mga_secondary_index_delta_ledger";
  return {};
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

bool ParseU64(std::string_view text, std::uint64_t* output) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
  *output = value;
  return true;
}

// An absent owned journal is empty. An unreadable or substituted source is not.
bool OpenSequenceJournal(const std::string& path, std::ifstream* input) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) return true;
  if (error || !std::filesystem::is_regular_file(status)) return false;
  input->open(path, std::ios::binary);
  return static_cast<bool>(*input);
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
  bool valid = true;
  bool found = false;
  std::uint64_t next = 0;
};

DurableEventSequenceState LoadDurableEventSequenceState(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path) {
  DurableEventSequenceState state;
  const std::string encoded_stream_path = EncodeCrudText(stream_path);
  std::ifstream input;
  if (!OpenSequenceJournal(EventSequenceAllocatorStorePath(context), &input)) {
    state.valid = false;
    return state;
  }
  // Deferred publishers may flush disjoint reservations out of order. Retried
  // flushes may repeat an exact record. Neither changes the high-water mark.
  using Range = std::pair<std::uint64_t, std::string>;
  std::map<std::string, std::map<std::uint64_t, Range>> ranges;
  std::string line;
  while (input.is_open() && std::getline(input, line)) {
    const auto fields = SplitTabs(line);
    if (input.eof() || fields.size() != 9 ||
        fields[0] != kEventSequenceAllocatorMagic || fields[1] != "RANGE") {
      state.valid = false;
      return state;
    }
    const std::string expected_path = OwnedSequenceStreamPath(context, fields[2]);
    const bool bootstrap = fields[7] == "bootstrap_store_scan";
    std::uint64_t first = 0, count = 0, next = 0;
    if (expected_path.empty() || fields[3] != EncodeCrudText(expected_path) ||
        (!bootstrap && fields[7] != "process_cache_after_durable_reservation" &&
         fields[7] != "durable_allocator_state") || fields[8] != (bootstrap ? "1" : "0") ||
        !ParseU64(fields[4], &first) || !ParseU64(fields[5], &count) ||
        !ParseU64(fields[6], &next) || first == 0 || count == 0 || next == 0 ||
        count > std::numeric_limits<std::uint64_t>::max() - first || first + count != next) {
      state.valid = false;
      return state;
    }
    auto& stream_ranges = ranges[fields[2]];
    const auto following = stream_ranges.lower_bound(first);
    if (following != stream_ranges.end() && following->first == first &&
        following->second.first == next && following->second.second == line) {
      continue;
    }
    if ((following != stream_ranges.end() && following->first < next) ||
        (following != stream_ranges.begin() && std::prev(following)->second.first > first)) {
      state.valid = false;
      return state;
    }
    stream_ranges.emplace_hint(following, first, Range{next, line});
    if (fields[2] == stream_kind && fields[3] == encoded_stream_path) {
      state.found = true;
      state.next = std::max(state.next, next);
    }
  }
  if (input.bad() || (input.is_open() && input.fail() && !input.eof())) state.valid = false;
  return state;
}

std::uint64_t NextReservedEventSequence(
    const EngineRequestContext& context,
    const std::string& stream_kind,
    const std::string& stream_path,
    std::uint64_t fallback_next) {
  if (fallback_next == 0) return 0;
  std::uint64_t next = fallback_next;
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
  if (!durable.valid) return 0;
  if (durable.found && durable.next != 0) {
    next = std::max(next, durable.next);
  }
  return next;
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
  if (stream_path != OwnedSequenceStreamPath(context, stream_kind)) {
    return RefuseEventSequenceReservation(
        context, stream_kind, stream_path, "stream_identity_invalid");
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
    if (!durable.valid) {
      return RefuseEventSequenceReservation(
          context, stream_kind, stream_path, "event_sequence_authority_invalid");
    }
    // A durable reservation record may lag an effect from an older interrupted
    // publisher. At cold admission validate actual stream authority as well;
    // never reuse a published sequence merely because its reservation is older.
    const auto observed_next = bootstrap_loader();
    if (observed_next == 0) {
      return RefuseEventSequenceReservation(
          context, stream_kind, stream_path, "event_sequence_bootstrap_invalid");
    }
    if (durable.found) {
      next_sequence = std::max(durable.next, observed_next);
      route = "durable_allocator_state";
    } else {
      next_sequence = observed_next;
      bootstrapped = true;
      route = "bootstrap_store_scan";
    }
  }
  if (next_sequence == 0) {
    return RefuseEventSequenceReservation(context, stream_kind, stream_path, "event_sequence_bootstrap_invalid");
  }
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
  std::uint64_t maximum=0;
  const auto scan=[&](const std::string& path) {
    std::vector<CrudRowVersionRecord> rows;ScopedRelationSummary summary;
    if(!DecodeScopedRowBinaryStore(path,&rows,&summary)||summary.malformed)return false;
    for(const auto& row:rows)maximum=std::max(maximum,row.event_sequence);
    return true;
  };
  if(!scan(RowStorePath(context)))return 0;
  try {
    for(const auto& [id,ordinal]:LoadMgaRelationLocators(context.database_path+".sb.mga_relation_scope")) {
      (void)ordinal;
      if(!scan(MgaScopedRelationPath(context,id,".rows"))||!scan(MgaScopedRelationPath(context,id,".rows.sbnr")))return 0;
    }
  } catch (const std::runtime_error&) { return 0; }
  return maximum==UINT64_MAX?0:maximum+1;
}

std::uint64_t NextRowEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(
      context, "row_versions", RowStorePath(context),
      ScanNextRowEventSequence(context));
}

std::uint64_t ScanNextIndexEventSequence(const EngineRequestContext& context) {
  std::uint64_t maximum=0;
  const auto scan=[&](const std::string& path) {
    std::vector<scratchbird::core::index::byte> bytes;
    std::vector<CrudIndexEntryRecord> entries;
    if(!ReadCompleteMgaBinaryFile(path,&bytes)||!DecodeScopedIndexBinaryBytes(bytes,&entries))return false;
    for(const auto& entry:entries)maximum=std::max(maximum,entry.event_sequence);
    return true;
  };
  if(!scan(IndexStorePath(context)))return 0;
  try {
    for(const auto& [id,ordinal]:LoadMgaRelationLocators(context.database_path+".sb.mga_relation_scope")) {
      (void)ordinal;
      if(!scan(MgaScopedRelationPath(context,id,".indexes"))||!scan(MgaScopedRelationPath(context,id,".indexes.sbnx")))return 0;
    }
  } catch (const std::runtime_error&) { return 0; }
  return maximum==UINT64_MAX?0:maximum+1;
}

std::uint64_t NextIndexEventSequence(const EngineRequestContext& context) {
  return NextReservedEventSequence(
      context, "index_entries", IndexStorePath(context),
      ScanNextIndexEventSequence(context));
}

std::uint64_t ScanNextMetadataEventSequence(
    const EngineRequestContext& context) {
  std::uint64_t max_sequence = 0;
  std::vector<std::string> records;
  if (!ReadCompleteMgaMetadataRecords(MetadataStorePath(context), &records)) return 0;
  for (const auto& line : records) {
    std::vector<std::string> fields;
    if (!DecodeMgaMetadataFields(line, &fields)) return 0;
    if (fields.size() >= 4 && fields[0] == kRowStoreMagic &&
        (fields[1] == "TABLE_METADATA" ||
         fields[1] == "TABLE_METADATA_RETIRED" ||
         fields[1] == kSealedTableMetadataKindV2 ||
         fields[1] == "INDEX_METADATA" ||
         fields[1] == "CONSTRAINT_MUTATION_BATCH" ||
         fields[1] == "BIGINT_IDENTITY_MIGRATION_BATCH" ||
         fields[1] == "INT32_IDENTITY_MIGRATION_BATCH" ||
         fields[1] == "TEXT_IDENTITY_MIGRATION_BATCH")) {
      std::uint64_t sequence = 0;
      if (!ParseU64(fields[3], &sequence) || sequence == 0) return 0;
      max_sequence = std::max(max_sequence, sequence);
    } else return 0;
  }
  for (const bool catalog : {true, false}) {
    std::ifstream input;
    if (!OpenSequenceJournal(context.database_path +
          (catalog ? ".sb.catalog_object_events" : ".sb.name_events.v2"), &input)) return 0;
    while (input.is_open() && input.peek() != std::char_traits<char>::eof()) {
      ApiBehaviorRecord frame;
      if (!ReadApiBehaviorRecord(input, &frame)) return 0;
      if (catalog) {
        CatalogLifecycleRecord record;
        if (!DecodeCatalogLifecycleFrame(frame, &record)) return 0;
      } else if (frame.object_kind == "name_registry.retire") {
        if (frame.target_object_uuid != frame.object_uuid || frame.state != "retired" ||
            !frame.target_database_uuid.is_nil() || !frame.target_schema_uuid.is_nil() ||
            !frame.default_name.empty() || !frame.payload.empty() || frame.deleted) return 0;
      } else {
        NameRegistryEntry entry;
        if (frame.object_kind != "name_registry.entry" || frame.state != "active" ||
            !frame.target_database_uuid.is_nil() || !frame.default_name.empty() || frame.deleted ||
            !DecodeNameRegistryEntry(frame.payload, &entry) ||
            entry.name_entry_uuid != frame.object_uuid || entry.object_uuid != frame.target_object_uuid ||
            entry.scope_uuid != frame.target_schema_uuid || entry.creator_tx != frame.creator_tx) return 0;
      }
      max_sequence = std::max(max_sequence, frame.event_sequence);
    }
    if (input.bad() || (input.is_open() && input.fail() && !input.eof())) return 0;
  }
  return max_sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : max_sequence + 1;
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
