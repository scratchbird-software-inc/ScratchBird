// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by crud_store.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.

bool CrudAssignmentsTouchOpaqueColumn(const CrudTableRecord& table,
                                      const std::vector<std::pair<std::string, EngineTypedValue>>& assignments) {
  for (const auto& [field, typed] : assignments) {
    if (CrudTypedValueIsOpaqueRenderOnly(typed) ||
        CrudColumnDescriptorIsOpaqueRenderOnly(CrudColumnDescriptorForName(table.columns, field))) {
      return true;
    }
  }
  return false;
}

CrudStoreResult LoadCrudState(const EngineRequestContext& context) {
  CrudStoreResult result;
  const auto path_status = ValidateCrudDatabasePath(context, "crud.load_state");
  if (path_status.error) { result.diagnostic = path_status; return result; }
  std::ifstream in(CrudEventPath(context), std::ios::binary);
  if (!in) { in.open(context.database_path, std::ios::binary); }
  if (!in) { result.ok = true; return result; }
  std::string line;
  while (std::getline(in, line)) {
    ++result.state.max_event_sequence;
    const std::uint64_t event_sequence = result.state.max_event_sequence;
    const auto magic_pos = line.find(kCrudMagic);
    if (magic_pos == std::string::npos) { continue; }
    if (magic_pos != 0) { line = line.substr(magic_pos); }
    const auto parts = Split(line, '\t');
    if (parts.size() < 2) { continue; }
    const auto& event = parts[1];
    if (event == "TX_BEGIN" && parts.size() >= 4) {
      const auto tx = ParseU64(parts[2]);
      result.state.transactions[tx] = "active";
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "TX_PREPARE" && parts.size() >= 3) {
      const auto tx = ParseU64(parts[2]);
      result.state.transactions[tx] = "prepared";
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "SAVEPOINT" && parts.size() >= 5) {
      const auto tx = ParseU64(parts[2]);
      result.state.savepoints[tx][HexDecode(parts[3])] = ParseU64(parts[4]);
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "RELEASE_SAVEPOINT" && parts.size() >= 4) {
      const auto tx = ParseU64(parts[2]);
      result.state.savepoints[tx].erase(HexDecode(parts[3]));
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "ROLLBACK_TO_SAVEPOINT" && parts.size() >= 4) {
      const auto tx = ParseU64(parts[2]);
      const auto sp = HexDecode(parts[3]);
      const auto tx_it = result.state.savepoints.find(tx);
      if (tx_it != result.state.savepoints.end()) {
        const auto sp_it = tx_it->second.find(sp);
        if (sp_it != tx_it->second.end()) {
          const auto marker = sp_it->second;
          auto after_marker = [tx, marker](const auto& record) { return record.creator_tx == tx && record.event_sequence > marker; };
          result.state.tables.erase(std::remove_if(result.state.tables.begin(), result.state.tables.end(), after_marker), result.state.tables.end());
          result.state.row_versions.erase(std::remove_if(result.state.row_versions.begin(), result.state.row_versions.end(), after_marker), result.state.row_versions.end());
          result.state.indexes.erase(std::remove_if(result.state.indexes.begin(), result.state.indexes.end(), after_marker), result.state.indexes.end());
          result.state.index_entries.erase(std::remove_if(result.state.index_entries.begin(), result.state.index_entries.end(), after_marker), result.state.index_entries.end());
          result.state.large_values.erase(std::remove_if(result.state.large_values.begin(), result.state.large_values.end(), after_marker), result.state.large_values.end());
          for (auto it = tx_it->second.begin(); it != tx_it->second.end();) {
            if (it->second > marker) { it = tx_it->second.erase(it); } else { ++it; }
          }
        }
      }
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "TX_COMMIT" && parts.size() >= 3) {
      const auto tx = ParseU64(parts[2]);
      result.state.transactions[tx] = "committed";
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "TX_ROLLBACK" && parts.size() >= 3) {
      const auto tx = ParseU64(parts[2]);
      result.state.transactions[tx] = "rolled_back";
      result.state.max_transaction_id = std::max(result.state.max_transaction_id, tx);
    } else if (event == "TABLE_CREATE" && parts.size() >= 6) {
      CrudTableRecord table;
      table.creator_tx = ParseU64(parts[2]);
      table.event_sequence = event_sequence;
      table.table_uuid = parts[3];
      table.default_name = HexDecode(parts[4]);
      table.columns = DecodeCrudPairs(parts[5]);
      result.state.tables.push_back(std::move(table));
    } else if (event == "TABLE_CREATE_V2" && parts.size() >= 10) {
      CrudTableRecord table;
      table.creator_tx = ParseU64(parts[2]);
      table.event_sequence = event_sequence;
      table.table_uuid = parts[3];
      table.default_name = HexDecode(parts[4]);
      table.columns = DecodeCrudPairs(parts[5]);
      table.temporary = ParseBool(parts[6]);
      table.temporary_scope = parts[7];
      table.temporary_session_uuid = parts[8];
      table.on_commit_action = parts[9];
      result.state.tables.push_back(std::move(table));
    } else if (event == "ROW_VERSION" && parts.size() >= 8) {
      CrudRowVersionRecord row;
      row.creator_tx = ParseU64(parts[2]);
      row.event_sequence = event_sequence;
      row.table_uuid = parts[3];
      row.row_uuid = parts[4];
      row.version_uuid = parts[5];
      row.deleted = parts[6] == "1";
      row.values = DecodeCrudPairs(parts[7]);
      row.sequence = ++result.state.max_sequence;
      result.state.row_versions.push_back(std::move(row));
    } else if (event == "ROW_VERSION_V2" && parts.size() >= 10) {
      CrudRowVersionRecord row;
      row.creator_tx = ParseU64(parts[2]);
      row.event_sequence = event_sequence;
      row.table_uuid = parts[3];
      row.row_uuid = parts[4];
      row.version_uuid = parts[5];
      row.deleted = parts[6] == "1";
      row.previous_version_uuid = parts[7];
      row.previous_sequence = ParseU64(parts[8]);
      row.values = DecodeCrudPairs(parts[9]);
      row.sequence = ++result.state.max_sequence;
      result.state.row_versions.push_back(std::move(row));
    } else if (event == "INDEX_CREATE" && parts.size() >= 8) {
      CrudIndexRecord index;
      index.creator_tx = ParseU64(parts[2]);
      index.event_sequence = event_sequence;
      index.index_uuid = parts[3];
      index.table_uuid = parts[4];
      index.column_name = HexDecode(parts[5]);
      index.profile = NormalizeCrudIndexProfile(parts[6]);
      index.family = CrudIndexFamilyForProfile(index.profile);
      index.default_name = HexDecode(parts[7]);
      index.key_envelopes.push_back(index.column_name);
      result.state.indexes.push_back(std::move(index));
    } else if (event == "INDEX_CREATE_V2" && parts.size() >= 15) {
      CrudIndexRecord index;
      index.creator_tx = ParseU64(parts[2]);
      index.event_sequence = event_sequence;
      index.index_uuid = parts[3];
      index.table_uuid = parts[4];
      index.profile = NormalizeCrudIndexProfile(parts[5]);
      index.family = parts[6].empty() ? CrudIndexFamilyForProfile(index.profile) : parts[6];
      index.default_name = HexDecode(parts[7]);
      index.column_name = HexDecode(parts[8]);
      index.key_envelopes = DecodeCrudList(parts[9]);
      index.include_columns = DecodeCrudList(parts[10]);
      index.predicate_kind = parts[11];
      index.predicate_column = HexDecode(parts[12]);
      index.predicate_value = HexDecode(parts[13]);
      index.unique = ParseBool(parts[14]);
      index.approximate = IsApproximateCrudIndexFamily(index.family);
      index.exact_fallback = index.approximate;
      result.state.indexes.push_back(std::move(index));
    } else if (event == "INDEX_ENTRY" && parts.size() >= 9) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = ParseU64(parts[2]);
      entry.event_sequence = event_sequence;
      entry.index_uuid = parts[3];
      entry.table_uuid = parts[4];
      entry.column_name = HexDecode(parts[5]);
      entry.family = kCrudIndexFamilyBtree;
      entry.entry_kind = "scalar";
      entry.key_value = HexDecode(parts[6]);
      entry.row_uuid = parts[7];
      entry.version_uuid = parts[8];
      entry.sequence = ++result.state.max_index_sequence;
      result.state.index_entries.push_back(std::move(entry));
    } else if (event == "INDEX_ENTRY_V2" && parts.size() >= 11) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = ParseU64(parts[2]);
      entry.event_sequence = event_sequence;
      entry.index_uuid = parts[3];
      entry.table_uuid = parts[4];
      entry.column_name = HexDecode(parts[5]);
      entry.family = parts[6];
      entry.entry_kind = parts[7];
      entry.key_value = HexDecode(parts[8]);
      entry.payload_value = HexDecode(parts[9]);
      entry.row_uuid = parts[10];
      if (parts.size() >= 12) { entry.version_uuid = parts[11]; }
      entry.sequence = ++result.state.max_index_sequence;
      result.state.index_entries.push_back(std::move(entry));
    } else if (event == "OVERFLOW_VALUE" && parts.size() >= 11) {
      CrudLargeValueRecord large;
      large.creator_tx = ParseU64(parts[2]);
      large.event_sequence = event_sequence;
      large.overflow_uuid = parts[3];
      large.table_uuid = parts[4];
      large.row_uuid = parts[5];
      large.version_uuid = parts[6];
      large.field_name = HexDecode(parts[7]);
      large.total_bytes = ParseU64(parts[8]);
      large.content_hash = parts[9];
      large.state = parts[10];
      result.state.large_values.push_back(std::move(large));
    } else if (event == "OVERFLOW_CHUNK" && parts.size() >= 7) {
      auto* large = FindMutableCrudLargeValue(&result.state, parts[3]);
      if (large != nullptr) {
        CrudLargeValueChunkRecord chunk;
        chunk.creator_tx = ParseU64(parts[2]);
        chunk.event_sequence = event_sequence;
        chunk.overflow_uuid = parts[3];
        chunk.ordinal = ParseU64(parts[4]);
        chunk.payload_fragment = HexDecode(parts[5]);
        chunk.checksum = ParseU64(parts[6]);
        large->chunks.push_back(std::move(chunk));
      }
    }
  }
  const auto large_value_status = MaterializeCrudLargeValues(&result.state);
  if (large_value_status.error) {
    result.ok = false;
    result.diagnostic = large_value_status;
    return result;
  }
  const auto authority_status = OverlayCrudTransactionAuthority(context, &result.state);
  if (authority_status.error) {
    result.ok = false;
    result.diagnostic = authority_status;
    return result;
  }
  const auto chain_status = ValidateCrudRowVersionChains(result.state);
  if (chain_status.error) {
    result.ok = false;
    result.diagnostic = chain_status;
    return result;
