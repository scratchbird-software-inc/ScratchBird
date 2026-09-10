// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "api_diagnostics.hpp"
#include "uuid.hpp"
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>

namespace scratchbird::engine::internal_api {
namespace {
bool Number(std::string_view text, std::uint64_t* value) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool Uuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
      scratchbird::core::uuid::UuidToString(parsed.value) == text;
}
std::uint64_t Checksum(std::string_view bytes) {
  std::uint64_t result = 1469598103934665603ull;
  for (unsigned char byte : bytes) { result ^= byte; result *= 1099511628211ull; }
  return result;
}
int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
struct Requested {
  const CrudRowVersionRecord* row = nullptr;
  std::string field;
  std::uint64_t bytes = 0, checksum = 0, next_chunk = 0, creator = 0;
  bool header = false, reclaimed = false;
  std::string payload;
  std::vector<std::string*> destinations;
};
}  // namespace

// SEARCH_KEY: SB_MGA_BOUNDED_VISIBLE_LARGE_VALUE_READER
EngineApiDiagnostic ExpandVisibleMgaLargeValuesBounded(
    const EngineRequestContext& context, std::vector<CrudRowVersionRecord>* rows,
    BoundedScopedRowReadControl* control, std::uint64_t retained_memory,
    const std::function<bool(std::uint64_t)>& creator_visible) {
  const auto refuse = [&](std::string detail, bool budget = false) {
    if (control) {
      control->refusal_detail = detail;
      control->failure_category = budget ? MgaHeapReadFailureCategoryV1::kResource
                                        : MgaHeapReadFailureCategoryV1::kCorruptStorage;
    }
    return MakeEngineApiDiagnostic(budget ? "RESOURCE.BUDGET_EXCEEDED" : "SB_ENGINE_API_INVALID_REQUEST",
        "mga.large_value.bounded_read", std::move(detail), true);
  };
  if (!rows || !control || !creator_visible) return refuse("large_value_read_authority_missing");
  constexpr std::string_view prefix = "SBMGA_LARGE_VALUE:";
  // Fixed record and stream buffers plus map/string bookkeeping are reserved
  // before reading. Payload and destination copies are charged separately.
  std::uint64_t memory = retained_memory;
  if (!HeapReadMemoryAdd(65536, &memory) || !ObserveBoundedHeapReadMemory(control, memory))
    return refuse("large_value_read_memory_limit", true);
  std::map<std::string, Requested> requested;
  for (auto& row : *rows) for (auto& [field, value] : row.values) {
    if (!value.starts_with(prefix)) {
      if (CrudValueIsLargeValueLocator(value)) return refuse("large_value_locator_encoding_unadmitted");
      continue;
    }
    const std::string_view tail(value.data() + prefix.size(), value.size() - prefix.size());
    const auto first = tail.find(':');
    const auto second = first == tail.npos ? tail.npos : tail.find(':', first + 1);
    std::uint64_t bytes = 0, checksum = 0;
    if (first != 36 || second == tail.npos || !Uuid(tail.substr(0, first)) ||
        !Number(tail.substr(first + 1, second - first - 1), &checksum) ||
        !Number(tail.substr(second + 1), &bytes)) return refuse("large_value_locator_invalid");
    std::uint64_t charge = 0;
    if (!HeapReadMemoryMultiply(bytes, 4, &charge) || !HeapReadMemoryAdd(4096, &charge) ||
        !HeapReadMemoryAdd(field.size(), &charge) ||
        !HeapReadMemoryAdd(charge, &memory) || !ObserveBoundedHeapReadMemory(control, memory))
      return refuse("large_value_payload_memory_limit", true);
    auto [entry, inserted] = requested.try_emplace(std::string(tail.substr(0, first)));
    auto& target = entry->second;
    if (inserted) {
      target.row = &row; target.field = field; target.bytes = bytes; target.checksum = checksum;
      if (bytes > target.payload.max_size()) return refuse("large_value_payload_extent_invalid", true);
      target.payload.reserve(static_cast<std::size_t>(bytes));
    } else if (target.row->table_uuid != row.table_uuid || target.row->row_uuid != row.row_uuid ||
               target.field != field || target.bytes != bytes || target.checksum != checksum) {
      return refuse("large_value_locator_owner_conflict");
    }
    target.destinations.push_back(&value);
  }
  if (requested.empty()) return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  std::ifstream input(context.database_path + ".sb.mga_large_values", std::ios::binary);
  if (!input) return refuse("large_value_store_missing");
  std::array<char, 16384> buffer{};
  while (input.peek() != std::char_traits<char>::eof()) {
    if (control->cancellation_requested && *control->cancellation_requested &&
        (*control->cancellation_requested)()) {
      control->cancellation_observed = true;
      control->failure_category = MgaHeapReadFailureCategoryV1::kCancellation;
      control->refusal_detail = "large_value_read_cancelled";
      return MakeEngineApiDiagnostic("PROCESS.CANCELLED", "mga.large_value.bounded_read",
                                      control->refusal_detail, true);
    }
    if (control->decoded_bytes >= control->maximum_bytes)
      return refuse("large_value_read_byte_limit", true);
    const auto remaining = control->maximum_bytes - control->decoded_bytes;
    const auto extent = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
    if (extent < 2) return refuse("large_value_read_byte_limit", true);
    input.getline(buffer.data(), extent);
    const auto consumed = static_cast<std::uint64_t>(input.gcount());
    if (input.fail() || input.eof() || consumed == 0)
      return refuse(extent < static_cast<std::streamsize>(buffer.size())
          ? "large_value_read_byte_limit" : "large_value_record_truncated_or_oversized",
          extent < static_cast<std::streamsize>(buffer.size()));
    if (control->decoded_bytes > control->maximum_bytes ||
        consumed > control->maximum_bytes - control->decoded_bytes)
      return refuse("large_value_read_byte_limit", true);
    control->decoded_bytes += consumed;
    if (control->runtime_observation) {
      if (!HeapReadMemoryAdd(consumed, &control->runtime_observation->storage_bytes_read))
        return refuse("large_value_read_byte_counter_overflow", true);
    }
    std::string_view line(buffer.data(), static_cast<std::size_t>(consumed - 1));
    std::array<std::string_view, 12> fields{};
    std::size_t count = 0;
    while (true) {
      if (count == fields.size()) return refuse("large_value_record_field_count_invalid");
      const auto tab = line.find('\t');
      fields[count++] = line.substr(0, tab);
      if (tab == line.npos) break;
      line.remove_prefix(tab + 1);
    }
    if (count < 4 || fields[0] != "SBMGA1") return refuse("large_value_record_header_invalid");
    const auto found = requested.find(std::string(fields[3]));
    if (found == requested.end()) continue;  // no unrelated payload materialization
    auto& target = found->second;
    std::uint64_t creator = 0;
    if (!Number(fields[2], &creator) || creator == 0) return refuse("large_value_creator_invalid");
    if (fields[1] == "LARGE_VALUE") {
      std::uint64_t bytes = 0, checksum = 0;
      if (count != 11 || target.header || fields[4] != target.row->table_uuid ||
          fields[5] != target.row->row_uuid || !Uuid(fields[6]) || fields[7] != target.field ||
          !Number(fields[8], &bytes) || !Number(fields[9], &checksum) ||
          bytes != target.bytes || checksum != target.checksum || fields[10] != "durable_uncommitted")
        return refuse("large_value_header_owner_or_extent_mismatch");
      target.header = true; target.creator = creator;
    } else if (fields[1] == "LARGE_VALUE_CHUNK") {
      std::uint64_t ordinal = 0, checksum = 0;
      if (count != 7 || !target.header || creator != target.creator ||
          !Number(fields[4], &ordinal) || ordinal != target.next_chunk ||
          !Number(fields[6], &checksum) || fields[5].size() % 2 != 0 || fields[5].size() > 4096 ||
          fields[5].size() / 2 > target.bytes - target.payload.size())
        return refuse("large_value_chunk_shape_or_order_invalid");
      std::array<char, 2048> chunk{};
      const auto size = fields[5].size() / 2;
      for (std::size_t i = 0; i < size; ++i) {
        const int high = Hex(fields[5][2 * i]), low = Hex(fields[5][2 * i + 1]);
        if (high < 0 || low < 0) return refuse("large_value_chunk_encoding_invalid");
        chunk[i] = static_cast<char>((high << 4) | low);
      }
      if (Checksum({chunk.data(), size}) != checksum) return refuse("large_value_chunk_checksum_mismatch");
      target.payload.append(chunk.data(), size);
      ++target.next_chunk;
    } else if (fields[1] == "LARGE_VALUE_RECLAIMED") {
      if (count < 9 || fields[4] != target.row->table_uuid || fields[5] != target.row->row_uuid)
        return refuse("large_value_reclaim_owner_invalid");
      if (creator_visible(creator)) target.reclaimed = true;
    } else return refuse("large_value_record_kind_invalid");
  }
  if (input.bad()) return refuse("large_value_read_io_failed");
  for (const auto& [id, target] : requested) {
    (void)id;
    if (!target.header || target.reclaimed || target.payload.size() != target.bytes ||
        Checksum(target.payload) != target.checksum) return refuse("large_value_payload_missing_or_reclaimed");
  }
  // Validation of every requested value precedes any caller-row replacement.
  for (const auto& [id, target] : requested) {
    (void)id;
    for (auto* value : target.destinations) *value = target.payload;
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}
}  // namespace scratchbird::engine::internal_api
