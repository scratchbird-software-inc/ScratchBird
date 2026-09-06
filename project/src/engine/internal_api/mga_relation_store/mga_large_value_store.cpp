// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_store_internal_support.hpp"

#include "api_diagnostics.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

// SEARCH_KEY: SB_ENGINE_MGA_LARGE_VALUE_STORE_IMPLEMENTATION_AUTHORITY
// Owns payload chunk persistence, locator expansion, and transaction-visible
// reclaim evidence. Large-value records are companion data; they never decide
// transaction finality or row visibility.
constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::size_t kMgaLargeValueChunkBytes = 2048;

std::string LargeValueStorePath(const EngineRequestContext& context) {
  return context.database_path + ".sb.mga_large_values";
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
    if (index != 0) line.push_back('\t');
    line += fields[index];
  }
  return line;
}

std::uint64_t ParseU64(const std::string& text,
                       std::uint64_t fallback = 0) {
  if (text.empty()) return fallback;
  try {
    return static_cast<std::uint64_t>(std::stoull(text));
  } catch (...) {
    return fallback;
  }
}

std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  if (!input) return lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

bool AppendLine(const std::string& path, const std::string& line) {
  std::ofstream output(path, std::ios::app | std::ios::binary);
  if (!output) return false;
  output << line << '\n';
  output.flush();
  return static_cast<bool>(output);
}

bool AppendLines(const std::string& path,
                 const std::vector<std::string>& lines,
                 std::uint64_t* stream_opens,
                 std::uint64_t* stream_flushes) {
  if (lines.empty()) return true;
  std::ofstream output(path, std::ios::app | std::ios::binary);
  if (!output) return false;
  if (stream_opens != nullptr) ++(*stream_opens);
  for (const auto& line : lines) output << line << '\n';
  output.flush();
  if (stream_flushes != nullptr) ++(*stream_flushes);
  return static_cast<bool>(output);
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  if ((encoded.size() % 2) != 0) return {};
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) return {};
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

std::uint64_t ChecksumText(const std::string& value) {
  std::uint64_t checksum = 1469598103934665603ull;
  for (unsigned char byte : value) {
    checksum ^= static_cast<std::uint64_t>(byte);
    checksum *= 1099511628211ull;
  }
  return checksum;
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

MgaLargeValueReclaimLoadResult LoadVisibleMgaLargeValueReclaimsImpl(
    const EngineRequestContext& context) {
  MgaLargeValueReclaimLoadResult result;
  result.diagnostic = OkDiagnostic();
  CrudState transaction_state;
  const auto authority =
      OverlayMgaTransactionAuthorityForStoreModule(context, &transaction_state, true);
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
  const auto reclaimed = LoadVisibleMgaLargeValueReclaimsImpl(context);
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

EngineApiDiagnostic ExpandMgaLargeValueLocatorsImpl(const EngineRequestContext& context,
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

bool RowsContainLargeValueLocatorsImpl(const std::vector<CrudRowVersionRecord>& rows) {
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
}  // namespace

MgaLargeValueReclaimLoadResult LoadVisibleMgaLargeValueReclaims(
    const EngineRequestContext& context) {
  return LoadVisibleMgaLargeValueReclaimsImpl(context);
}

MgaTemporaryLargeValueRecoveryResult ClassifyMgaTemporaryLargeValueRecovery(
    const EngineRequestContext& context,
    const std::set<std::string>& temporary_tables,
    const std::map<std::uint64_t, std::string>& transaction_states) {
  MgaTemporaryLargeValueRecoveryResult result;
  std::set<std::string> committed_large_values;
  std::set<std::string> reclaimed_large_values;
  auto classify_event = [&](const std::uint64_t creator_tx) {
    if (creator_tx == 0) return std::string("committed");
    const auto found = transaction_states.find(creator_tx);
    if (found == transaction_states.end()) {
      ++result.fenced_event_count;
      return std::string("fenced");
    }
    if (found->second == "committed" || found->second == "archived") {
      return std::string("committed");
    }
    if (found->second == "rolled_back") {
      ++result.rolled_back_event_count;
      return std::string("rolled_back");
    }
    ++result.active_or_unresolved_event_count;
    return std::string("active_or_unresolved");
  };

  for (const auto& line : ReadLines(LargeValueStorePath(context))) {
    const auto fields = SplitTabs(line);
    if (fields.size() >= 11 && fields[0] == kRowStoreMagic &&
        fields[1] == "LARGE_VALUE") {
      if (temporary_tables.count(fields[4]) == 0) continue;
      if (classify_event(ParseU64(fields[2])) == "committed") {
        committed_large_values.insert(fields[3]);
      }
    } else if (fields.size() >= 9 && fields[0] == kRowStoreMagic &&
               fields[1] == "LARGE_VALUE_RECLAIMED") {
      if (temporary_tables.count(fields[4]) == 0) continue;
      if (classify_event(ParseU64(fields[2])) == "committed") {
        reclaimed_large_values.insert(fields[3]);
      }
    }
  }
  result.reclaimed_large_value_count = reclaimed_large_values.size();
  for (const auto& overflow_uuid : committed_large_values) {
    if (reclaimed_large_values.count(overflow_uuid) == 0) {
      ++result.orphaned_large_value_count;
    }
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

bool RowsContainLargeValueLocators(
    const std::vector<CrudRowVersionRecord>& rows) {
  return RowsContainLargeValueLocatorsImpl(rows);
}

EngineApiDiagnostic ExpandMgaLargeValueLocators(
    const EngineRequestContext& context,
    std::vector<CrudRowVersionRecord>* rows) {
  return ExpandMgaLargeValueLocatorsImpl(context, rows);
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

}  // namespace scratchbird::engine::internal_api
