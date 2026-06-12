// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
    return MakeInvalidRequestDiagnostic("crud.transaction_authority", "active_local_transaction_required");
  }

  if (IsTerminalTransactionState(lookup.entry.state)) {
    return MakeInvalidRequestDiagnostic("crud.transaction_authority", "terminal_transaction_cannot_mutate");
  }

  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ValidateCrudRowVersionChains(const CrudState& state) {
  std::map<std::string, CrudRowVersionRecord> by_version_uuid;
  for (const auto& row : state.row_versions) {
    if (row.version_uuid.empty()) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "row_version_uuid_required");
    }
    if (by_version_uuid.count(row.version_uuid) != 0) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "duplicate_row_version_uuid");
    }
    by_version_uuid[row.version_uuid] = row;
  }
  for (const auto& row : state.row_versions) {
    if (row.previous_version_uuid.empty()) { continue; }
    const auto previous = by_version_uuid.find(row.previous_version_uuid);
    if (previous == by_version_uuid.end()) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "previous_row_version_missing");
    }
    if (previous->second.table_uuid != row.table_uuid || previous->second.row_uuid != row.row_uuid) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "previous_row_version_wrong_chain");
    }
    if (previous->second.sequence >= row.sequence) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "previous_row_version_not_older");
    }
    if (row.previous_sequence != 0 && previous->second.sequence != row.previous_sequence) {
      return MakeInvalidRequestDiagnostic("crud.row_version_chain", "previous_row_version_sequence_mismatch");
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::uint64_t ParseU64(const std::string& value) {
  try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) { return 0; }
}

bool ParseBool(const std::string& value) { return value == "1" || value == "true" || value == "TRUE"; }

std::uint64_t CrudLargeValueChecksum(const std::string& value) {
  std::uint64_t checksum = 0;
  for (unsigned char c : value) {
    checksum = (checksum * 131) + static_cast<std::uint64_t>(c);
  }
  return checksum;
}

std::string CrudLargeValueHash(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ULL;
  }
  return "fnv1a64_" + std::to_string(hash);
}

std::string MakeCrudLargeValueLocator(const std::string& overflow_uuid,
                                      const std::string& content_hash,
                                      std::uint64_t total_bytes) {
  return std::string(kCrudLargeValueLocatorPrefix) + overflow_uuid + ":" + content_hash + ":" +
         std::to_string(total_bytes);
}

bool ParseCrudLargeValueLocator(const std::string& value,
                                std::string* overflow_uuid,
                                std::string* content_hash,
                                std::uint64_t* total_bytes) {
  if (!StartsWith(value, kCrudLargeValueLocatorPrefix)) {
    return false;
  }
  const std::string body = value.substr(std::string(kCrudLargeValueLocatorPrefix).size());
  const auto first = body.find(':');
  const auto second = first == std::string::npos ? std::string::npos : body.find(':', first + 1);
  if (first == std::string::npos || second == std::string::npos) {
    return false;
  }
  if (overflow_uuid != nullptr) { *overflow_uuid = body.substr(0, first); }
  if (content_hash != nullptr) { *content_hash = body.substr(first + 1, second - first - 1); }
  if (total_bytes != nullptr) { *total_bytes = ParseU64(body.substr(second + 1)); }
  return true;
}

CrudLargeValueRecord* FindMutableCrudLargeValue(CrudState* state, const std::string& overflow_uuid) {
  if (state == nullptr) { return nullptr; }
  auto found = std::find_if(state->large_values.begin(),
                            state->large_values.end(),
                            [&](const CrudLargeValueRecord& record) {
                              return record.overflow_uuid == overflow_uuid;
                            });
  return found == state->large_values.end() ? nullptr : &(*found);
}

const CrudLargeValueRecord* FindCrudLargeValue(const CrudState& state, const std::string& overflow_uuid) {
  auto found = std::find_if(state.large_values.begin(),
                            state.large_values.end(),
                            [&](const CrudLargeValueRecord& record) {
                              return record.overflow_uuid == overflow_uuid;
                            });
  return found == state.large_values.end() ? nullptr : &(*found);
}

bool ReconstructCrudLargeValue(const CrudLargeValueRecord& record,
                               const std::string& expected_hash,
                               std::uint64_t expected_total_bytes,
                               std::string* reconstructed,
                               std::string* reason) {
  if (record.state != "durable_uncommitted" && record.state != "committed_visible") {
    if (reason != nullptr) { *reason = "overflow_value_state_not_visible"; }
    return false;
  }
  if (record.content_hash != expected_hash || record.total_bytes != expected_total_bytes) {
    if (reason != nullptr) { *reason = "overflow_value_locator_metadata_mismatch"; }
    return false;
  }
  std::vector<CrudLargeValueChunkRecord> chunks = record.chunks;
  std::sort(chunks.begin(), chunks.end(), [](const auto& left, const auto& right) {
    return left.ordinal < right.ordinal;
  });
  std::string value;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].ordinal != i) {
      if (reason != nullptr) { *reason = "overflow_chunk_ordinal_gap"; }
      return false;
    }
    if (CrudLargeValueChecksum(chunks[i].payload_fragment) != chunks[i].checksum) {
      if (reason != nullptr) { *reason = "overflow_chunk_checksum_mismatch"; }
      return false;
    }
    value += chunks[i].payload_fragment;
  }
  if (value.size() != expected_total_bytes || CrudLargeValueHash(value) != expected_hash) {
    if (reason != nullptr) { *reason = "overflow_value_hash_mismatch"; }
    return false;
  }
  if (reconstructed != nullptr) { *reconstructed = std::move(value); }
  return true;
}

EngineApiDiagnostic MaterializeCrudLargeValues(CrudState* state) {
  if (state == nullptr) {
    return MakeInvalidRequestDiagnostic("crud.large_value", "state_required");
  }
  for (auto& row : state->row_versions) {
    for (auto& [field, value] : row.values) {
      std::string overflow_uuid;
      std::string content_hash;
      std::uint64_t total_bytes = 0;
      if (!ParseCrudLargeValueLocator(value, &overflow_uuid, &content_hash, &total_bytes)) {
        continue;
      }
      const auto* record = FindCrudLargeValue(*state, overflow_uuid);
      if (record == nullptr) {
        return MakeInvalidRequestDiagnostic("crud.large_value", "overflow_value_missing");
      }
      std::string reconstructed;
      std::string reason;
      if (!ReconstructCrudLargeValue(*record, content_hash, total_bytes, &reconstructed, &reason)) {
        return MakeInvalidRequestDiagnostic("crud.large_value", reason.empty() ? "overflow_value_invalid" : reason);
      }
      value = std::move(reconstructed);
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string EncodeCrudList(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (std::size_t i = 0; i < values.size(); ++i) { pairs.push_back({std::to_string(i), values[i]}); }
  return EncodeCrudPairs(pairs);
}

std::vector<std::string> DecodeCrudList(const std::string& encoded) {
  std::vector<std::string> values;
  for (const auto& [ignored, value] : DecodeCrudPairs(encoded)) { values.push_back(value); }
  return values;
}

bool IsDirectiveEnvelope(const std::string& envelope) {
  return envelope == "unique" || StartsWith(envelope, "include:") || StartsWith(envelope, "where_eq:");
}

std::vector<std::string> CrudIndexKeyColumns(const CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    if (!IsDirectiveEnvelope(envelope)) { columns.push_back(envelope); }
  }
  if (columns.empty() && !index.column_name.empty()) { columns.push_back(index.column_name); }
  return columns;
}

// SB_MGA_TEMPORARY_OBJECT_SCOPE_AUTHORITY
bool TableVisibleInCurrentTemporaryScope(const CrudTableRecord& table, const EngineRequestContext& context) {
  if (!table.temporary) { return true; }
  if (table.temporary_scope == "global") { return true; }
  const std::string session_uuid = SessionUuidOrEmpty(context);
  return !session_uuid.empty() && table.temporary_session_uuid == session_uuid;
}

void FilterTemporaryObjectsForSession(const EngineRequestContext& context, CrudState* state) {
  std::set<std::string> purged_tables;
  std::vector<CrudTableRecord> retained_tables;
  retained_tables.reserve(state->tables.size());
  for (const auto& table : state->tables) {
    if (TableVisibleInCurrentTemporaryScope(table, context)) {
      retained_tables.push_back(table);
    } else {
      purged_tables.insert(table.table_uuid);
    }
  }
  state->tables = std::move(retained_tables);
  if (purged_tables.empty()) { return; }
  state->row_versions.erase(std::remove_if(state->row_versions.begin(),
                                           state->row_versions.end(),
                                           [&purged_tables](const CrudRowVersionRecord& row) {
                                             return purged_tables.count(row.table_uuid) != 0;
