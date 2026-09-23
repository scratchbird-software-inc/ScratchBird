// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "api_diagnostics.hpp"
#include "mga_relation_store/mga_large_value_codec.hpp"
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
std::uint64_t Checksum(std::string_view bytes) {
  std::uint64_t result = 1469598103934665603ull;
  for (unsigned char byte : bytes) { result ^= byte; result *= 1099511628211ull; }
  return result;
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
  // Fixed record and stream buffers plus map/string bookkeeping are reserved
  // before reading. Payload and destination copies are charged separately.
  std::uint64_t memory = retained_memory;
  if (!HeapReadMemoryAdd(65536, &memory) || !ObserveBoundedHeapReadMemory(control, memory))
    return refuse("large_value_read_memory_limit", true);
  std::map<EngineUuid, Requested> requested;
  for (auto& row : *rows) for (auto& [field, value] : row.values) {
    if (!IsMgaLargeValueLocator(value)) {
      if (CrudValueIsLargeValueLocator(value) || value.starts_with("SBMGA_LARGE_VALUE:")) return refuse("large_value_locator_encoding_unadmitted");
      continue;
    }
    EngineUuid overflow_uuid;
    std::uint64_t bytes = 0, checksum = 0;
    if (!ReadMgaLargeValueLocator(value, &overflow_uuid, &checksum, &bytes))
      return refuse("large_value_locator_invalid");
    std::uint64_t charge = 0;
    if (!HeapReadMemoryMultiply(bytes, 4, &charge) || !HeapReadMemoryAdd(4096, &charge) ||
        !HeapReadMemoryAdd(field.size(), &charge) ||
        !HeapReadMemoryAdd(charge, &memory) || !ObserveBoundedHeapReadMemory(control, memory))
      return refuse("large_value_payload_memory_limit", true);
    auto [entry, inserted] = requested.try_emplace(overflow_uuid);
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
    if (remaining < 16) return refuse("large_value_read_byte_limit", true);
    input.read(buffer.data(), 16);
    if (input.gcount() != 16) return refuse("large_value_record_truncated");
    const std::span<const std::uint8_t> header(reinterpret_cast<const std::uint8_t*>(buffer.data()), 16);
    std::size_t cursor = 8;
    std::uint64_t payload_bytes = 0;
    if (!std::string_view(buffer.data(), 8).starts_with(kMgaMetadataRecordMagic) ||
        !ReadBinaryU64(header, &cursor, &payload_bytes) || payload_bytes > buffer.size() - 48)
      return refuse("large_value_record_extent_invalid");
    const auto consumed = payload_bytes + 48;
    if (consumed > remaining) return refuse("large_value_read_byte_limit", true);
    input.read(buffer.data() + 16, static_cast<std::streamsize>(consumed - 16));
    if (input.gcount() != static_cast<std::streamsize>(consumed - 16))
      return refuse("large_value_record_truncated");
    if (control->decoded_bytes > control->maximum_bytes ||
        consumed > control->maximum_bytes - control->decoded_bytes)
      return refuse("large_value_read_byte_limit", true);
    control->decoded_bytes += consumed;
    if (control->runtime_observation) {
      if (!HeapReadMemoryAdd(consumed, &control->runtime_observation->storage_bytes_read))
        return refuse("large_value_read_byte_counter_overflow", true);
    }
    std::vector<std::string> fields;
    if (!DecodeMgaMetadataFields(std::string_view(buffer.data(), consumed), &fields) ||
        !ValidateMgaLargeValueFields(fields)) return refuse("large_value_record_invalid");
    const auto count = fields.size();
    EngineUuid overflow_uuid;
    if (!ReadMetadataUuid(fields[3], &overflow_uuid)) return refuse("large_value_identity_invalid");
    const auto found = requested.find(overflow_uuid);
    if (found == requested.end()) continue;  // no unrelated payload materialization
    auto& target = found->second;
    std::uint64_t creator = 0;
    if (!Number(fields[2], &creator) || creator == 0) return refuse("large_value_creator_invalid");
    EngineUuid table_uuid, row_uuid, version_uuid;
    if (fields[1] != "LARGE_VALUE_CHUNK" &&
        (!ReadMetadataUuid(fields[4], &table_uuid) || !ReadMetadataUuid(fields[5], &row_uuid) ||
         !ReadMetadataUuid(fields[6], &version_uuid))) return refuse("large_value_owner_invalid");
    if (fields[1] == "LARGE_VALUE") {
      std::uint64_t bytes = 0, checksum = 0;
      if (count != 11 || target.header || table_uuid != target.row->table_uuid ||
          row_uuid != target.row->row_uuid || fields[7] != target.field ||
          !Number(fields[8], &bytes) || !Number(fields[9], &checksum) ||
          bytes != target.bytes || checksum != target.checksum || fields[10] != "durable_uncommitted")
        return refuse("large_value_header_owner_or_extent_mismatch");
      target.header = true; target.creator = creator;
    } else if (fields[1] == "LARGE_VALUE_CHUNK") {
      std::uint64_t ordinal = 0, checksum = 0;
      if (count != 7 || !target.header || creator != target.creator ||
          !Number(fields[4], &ordinal) || ordinal != target.next_chunk ||
          !Number(fields[6], &checksum) || fields[5].size() > 2048 ||
          fields[5].size() > target.bytes - target.payload.size())
        return refuse("large_value_chunk_shape_or_order_invalid");
      if (Checksum(fields[5]) != checksum) return refuse("large_value_chunk_checksum_mismatch");
      target.payload.append(fields[5]);
      ++target.next_chunk;
    } else if (fields[1] == "LARGE_VALUE_RECLAIMED") {
      if (count < 9 || table_uuid != target.row->table_uuid || row_uuid != target.row->row_uuid)
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
