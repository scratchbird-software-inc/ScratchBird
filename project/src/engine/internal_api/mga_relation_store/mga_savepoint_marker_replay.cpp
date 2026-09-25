// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_savepoint_marker_replay.hpp"
#include "mga_savepoint_marker_codec.hpp"
#include "crud_support/crud_store.hpp"
#include <algorithm>
#include <charconv>
#include <ranges>
#include <string_view>
namespace scratchbird::engine::internal_api::savepoint_replay {
// SEARCH_KEY: SB_ENGINE_MGA_SAVEPOINT_MARKER_REPLAY_IMPLEMENTATION_AUTHORITY
// Decode marker history into transaction-local rollback ranges. This module
// performs no durable writes and owns no transaction finality authority.
namespace {
constexpr const char* kRowStoreMagic = "SBMGA1";
constexpr std::string_view kDmlUpdateStatementSavepointCreateKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_CREATE_V1";
constexpr std::string_view kDmlUpdateStatementSavepointRollbackKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_ROLLBACK_V1";
constexpr std::string_view kDmlUpdateStatementSavepointReleaseKind =
    "DML_UPDATE_STATEMENT_SAVEPOINT_RELEASE_V1";

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

bool ParseU64(const std::string& text, std::uint64_t* value) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

int HexValue(const char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string DecodeCrudTextLocal(const std::string& encoded) {
  if ((encoded.size() % 2) != 0) { return {}; }
  std::string decoded;
  decoded.reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const int high = HexValue(encoded[index]);
    const int low = HexValue(encoded[index + 1]);
    if (high < 0 || low < 0) { return {}; }
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
}

bool ApplyMarkerRecord(const MgaSavepointMarkerRecord& record, SavepointParsedState* state) {
  if (!state || record.transaction == 0 || record.kind < 1 || record.kind > 3) return false;
  const auto tx = record.transaction;
  const auto name = record.uuid_identity ? MgaSavepointUuidKey(record.uuid) : record.identity;
  if (name.empty()) return false;
  const bool create = record.kind == 1, release = record.kind == 2, rollback = record.kind == 3;
  SavepointCutoffs cutoffs;
  cutoffs.row_event_sequence = record.cutoffs[0];
  cutoffs.metadata_event_sequence = record.cutoffs[1];
  cutoffs.index_event_sequence = record.cutoffs[2];
  if (create) {
    if (state->next_creation_ordinal == UINT64_MAX) {
      state->marker_authority_corrupt = true;
      return false;
    }
    cutoffs.creation_ordinal = state->next_creation_ordinal++;
    if (auto* observed = state->observation;
        observed && observed->transaction == tx && observed->identity == name) {
      if (observed->require_unique_identity) {
        if (observed->creation_ordinal) {
          state->marker_authority_corrupt = true;
          return false;
        }
        observed->creation_ordinal = cutoffs.creation_ordinal;
      }
      if (cutoffs.creation_ordinal == observed->creation_ordinal) {
        observed->lifecycle = MgaSavepointMarkerLifecycle::active;
        observed->cutoffs = cutoffs;
      } else if (observed->lifecycle == MgaSavepointMarkerLifecycle::active) {
        observed->lifecycle = MgaSavepointMarkerLifecycle::invalidated;
      }
    }
    state->active_savepoints[tx][name] = cutoffs;
  } else if (release) {
    const auto tx_it = state->active_savepoints.find(tx);
    if (auto* observed = state->observation;
        observed && observed->transaction == tx && observed->identity == name &&
        observed->lifecycle == MgaSavepointMarkerLifecycle::active &&
        tx_it != state->active_savepoints.end() && tx_it->second.contains(name) &&
        tx_it->second.at(name).creation_ordinal == observed->creation_ordinal) {
      observed->lifecycle = MgaSavepointMarkerLifecycle::released;
    }
    if (tx_it == state->active_savepoints.end() ||
        tx_it->second.erase(name) == 0) {
      state->marker_authority_corrupt = true;
    }
  } else if (rollback) {
    SavepointRollbackRange range;
    range.cutoffs = cutoffs;
    range.row_upper_event_sequence = record.upper[0];
    range.metadata_upper_event_sequence = record.upper[1];
    range.index_upper_event_sequence = record.upper[2];
    if (range.row_upper_event_sequence < cutoffs.row_event_sequence ||
        range.metadata_upper_event_sequence < cutoffs.metadata_event_sequence ||
        range.index_upper_event_sequence < cutoffs.index_event_sequence) return false;
    auto tx_it = state->active_savepoints.find(tx);
    if (tx_it == state->active_savepoints.end() ||
        !tx_it->second.contains(name)) {
      state->marker_authority_corrupt = true;
      return false;
    }
    const auto target = tx_it->second.at(name);
    if (target.row_event_sequence != cutoffs.row_event_sequence ||
        target.metadata_event_sequence != cutoffs.metadata_event_sequence ||
        target.index_event_sequence != cutoffs.index_event_sequence) {
      state->marker_authority_corrupt = true;
      return false;
    }
    if (auto* observed = state->observation;
        observed && observed->transaction == tx &&
        observed->lifecycle == MgaSavepointMarkerLifecycle::active) {
      if (observed->creation_ordinal > target.creation_ordinal)
        observed->lifecycle = MgaSavepointMarkerLifecycle::invalidated;
      else if (observed->creation_ordinal == target.creation_ordinal)
        observed->rolled_back = true;
    }
    std::erase_if(tx_it->second, [&](const auto& entry) {
      return entry.second.creation_ordinal > target.creation_ordinal;
    });
    state->rollback_ranges[tx].push_back(range);
  }
  return true;
}

bool ApplySavepointRecordLine(const std::string& line,
                              SavepointParsedState* state) {
  if (state == nullptr) return false;
  const auto fields = SplitTabs(line);
  if (fields.size() < 2 || fields[0] != kRowStoreMagic) return false;
  const std::string& kind = fields[1];
  const bool update_statement_create =
      kind == kDmlUpdateStatementSavepointCreateKind;
  const bool update_statement_rollback =
      kind == kDmlUpdateStatementSavepointRollbackKind;
  const bool update_statement_release =
      kind == kDmlUpdateStatementSavepointReleaseKind;
  if (update_statement_create || update_statement_rollback ||
      update_statement_release) {
    // UPDATE statement identity/barrier authority is binary MGA durability.
    // A historical host-text record is never admitted as a compatibility
    // authority and forces current transaction reads to fail closed.
    state->update_statement_authority_corrupt = true;
    return false;
  }
  const bool create = kind == "SAVEPOINT";
  const bool release = kind == "RELEASE_SAVEPOINT";
  const bool rollback = kind == "ROLLBACK_TO_SAVEPOINT";
  if ((!create && !release && !rollback) ||
      fields.size() != (rollback ? 10u : 7u)) return false;
  std::uint64_t tx = 0;
  if (!ParseU64(fields[2], &tx) || tx == 0) return false;
  const std::string name = DecodeCrudTextLocal(fields[3]);
  if (name.empty() || EncodeCrudText(name) != fields[3]) return false;
  // Historical SQL labels are readable, but native marker identities never
  // enter through the text/hex compatibility decoder.
  if (name.find('\0') != std::string::npos) return false;
  SavepointCutoffs cutoffs;
  if (!ParseU64(fields[4], &cutoffs.row_event_sequence) ||
      !ParseU64(fields[5], &cutoffs.metadata_event_sequence) ||
      !ParseU64(fields[6], &cutoffs.index_event_sequence)) return false;
  MgaSavepointMarkerRecord record;
  record.kind = create ? 1 : release ? 2 : 3;
  record.transaction = tx;
  record.identity = name;
  record.cutoffs[0] = cutoffs.row_event_sequence;
  record.cutoffs[1] = cutoffs.metadata_event_sequence;
  record.cutoffs[2] = cutoffs.index_event_sequence;
  if (rollback)
    for (unsigned i = 0; i < 3; ++i)
      if (!ParseU64(fields[7 + i], &record.upper[i])) return false;
  return ApplyMarkerRecord(record, state);
}

} // namespace

void NormalizeSavepointRowRollbackRanges(SavepointParsedState* state) {
  if (state == nullptr) return;
  state->normalized_row_rollback_ranges.clear();
  for (const auto& [transaction, source_ranges] : state->rollback_ranges) {
    auto& normalized = state->normalized_row_rollback_ranges[transaction];
    normalized.reserve(source_ranges.size());
    for (const auto& range : source_ranges) {
      if (range.row_upper_event_sequence <=
          range.cutoffs.row_event_sequence) {
        continue;
      }
      normalized.push_back({range.cutoffs.row_event_sequence,
                            range.row_upper_event_sequence});
    }
    std::ranges::sort(normalized);
    std::size_t write = 0;
    for (const auto& range : normalized) {
      if (write != 0 && range.first <= normalized[write - 1].second) {
        normalized[write - 1].second =
            std::max(normalized[write - 1].second, range.second);
      } else {
        normalized[write++] = range;
      }
    }
    normalized.resize(write);
  }
  state->row_rollback_ranges_normalized = true;
}

void ConsumeMarkerBytes(std::string* pending, SavepointParsedState* state,
                        bool final) {
  std::size_t consumed = 0;
  while (consumed < pending->size() && !state->marker_authority_corrupt) {
    const std::string_view remaining(pending->data() + consumed, pending->size() - consumed);
    if (static_cast<unsigned char>(remaining.front()) == kMgaSavepointFrameLead) {
      const auto size = MgaSavepointMarkerFrameSize(remaining);
      if (size == UINT32_MAX) { state->marker_authority_corrupt = true; break; }
      if (size == 0 || remaining.size() < size) break;
      MgaSavepointMarkerRecord record;
      if (!DecodeMgaSavepointMarker(remaining.substr(0, size), &record)) {
        state->marker_authority_corrupt = true;
        break;
      }
      if (!ApplyMarkerRecord(record, state)) state->marker_authority_corrupt = true;
      consumed += size;
    } else {
      const auto end = remaining.find('\n');
      if (end == std::string_view::npos) break;
      if (!ApplySavepointRecordLine(std::string(remaining.substr(0, end)), state))
        state->marker_authority_corrupt = true;
      consumed += end + 1;
    }
  }
  pending->erase(0, consumed);
  if ((final && !pending->empty()) || pending->size() > kMgaSavepointFrameMaximum)
    state->marker_authority_corrupt = true;
}

} // namespace scratchbird::engine::internal_api::savepoint_replay
