// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"

namespace scratchbird::engine::internal_api {
inline constexpr std::string_view kScopedIndexBinaryBatchMagic = "SBMIBIN1";
inline constexpr std::uint16_t kScopedIndexBinaryVersion = 2;

// Version 2 keeps the batch envelope and TEXT key/payload fields, but stores
// each table/index/row/version identity as exactly sixteen native UUID bytes.
// Format decoding never decides transaction visibility or finality.
inline bool AppendScopedExactIndexBinaryBatch(
    std::string* out, const MgaExactIndexEntryAppendBatch& batch,
    std::uint64_t creator_tx, std::uint64_t first_event_sequence) {
  if (!out || batch.entries.empty() ||
      batch.entries.size() - 1 > UINT64_MAX - first_event_sequence) return false;
  const auto table = batch.index.table_uuid.is_nil() ? batch.table_uuid : batch.index.table_uuid;
  if (!batch.table_uuid.is_nil() && batch.table_uuid != table) return false;
  std::string staged(kScopedIndexBinaryBatchMagic);
  AppendBinaryU16(&staged, kScopedIndexBinaryVersion);
  AppendBinaryU16(&staged, 0);
  AppendBinaryU64(&staged, batch.entries.size());
  AppendBinaryU64(&staged, creator_tx);
  AppendBinaryU64(&staged, first_event_sequence);
  if (!AppendBinaryEngineUuid(&staged, table) ||
      !AppendBinaryEngineUuid(&staged, batch.index.index_uuid) ||
      !AppendBinaryString(&staged, batch.index.column_name) ||
      !AppendBinaryString(&staged, batch.index.family) ||
      !AppendBinaryString(&staged, batch.entry_kind.empty() ? "exact" : batch.entry_kind)) return false;
  for (const auto& entry : batch.entries) {
    if (entry.encoded_key.empty() ||
        !AppendBinaryString(&staged, entry.encoded_key) ||
        !AppendBinaryString(&staged, entry.payload_value) ||
        !AppendBinaryEngineUuid(&staged, entry.row_uuid) ||
        !AppendBinaryEngineUuid(&staged, entry.version_uuid)) return false;
  }
  if (staged.size() > out->max_size() - out->size()) return false;
  out->append(staged);
  return true;
}

// General materialization uses the same raw16 format as the exact fast path.
inline bool AppendScopedIndexEntryBinaryRecord(std::string* out,
    std::uint64_t creator_tx, std::uint64_t event_sequence,
    const EngineUuid& index_uuid, const EngineUuid& table_uuid,
    std::string_view column_name, std::string_view family,
    std::string_view entry_kind, std::string_view key,
    std::string_view payload, const EngineUuid& row_uuid,
    const EngineUuid& version_uuid) {
  MgaExactIndexEntryAppendBatch batch;
  batch.table_uuid = table_uuid;
  batch.index.table_uuid = table_uuid;
  batch.index.index_uuid = index_uuid;
  batch.index.column_name = column_name;
  batch.index.family = family;
  batch.entry_kind = entry_kind;
  batch.entries.push_back({std::string(key), std::string(payload), row_uuid, version_uuid});
  return AppendScopedExactIndexBinaryBatch(out, batch, creator_tx, event_sequence);
}

// System storage accepts binary16 identities only. Refuse unsupported TEXT
// formats and mixed streams before publishing any decoded entries.
inline bool DecodeScopedIndexBinaryBytes(std::span<const std::uint8_t> bytes,
    std::vector<CrudIndexEntryRecord>* entries) {
  if (!entries) return false;
  std::vector<CrudIndexEntryRecord> staged;
  std::size_t cursor = 0;
  while (cursor < bytes.size()) {
    if (bytes.size() - cursor < kScopedIndexBinaryBatchMagic.size() ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + cursor),
                         kScopedIndexBinaryBatchMagic.size()) != kScopedIndexBinaryBatchMagic) return false;
    cursor += kScopedIndexBinaryBatchMagic.size();
    std::uint16_t version = 0, flags = 0;
    std::uint64_t count = 0, creator = 0, first = 0;
    if (!ReadBinaryU16(bytes, &cursor, &version) ||
        !ReadBinaryU16(bytes, &cursor, &flags) ||
        !ReadBinaryU64(bytes, &cursor, &count) ||
        !ReadBinaryU64(bytes, &cursor, &creator) ||
        !ReadBinaryU64(bytes, &cursor, &first) || flags != 0 ||
        version != kScopedIndexBinaryVersion || count == 0) return false;
    EngineUuid table, index;
    std::string column, family, kind;
    if (!ReadBinaryEngineUuid(bytes, &cursor, &table) ||
        !ReadBinaryEngineUuid(bytes, &cursor, &index) ||
        !ReadBinaryString(bytes, &cursor, &column) ||
        !ReadBinaryString(bytes, &cursor, &family) ||
        !ReadBinaryString(bytes, &cursor, &kind) || kind.empty()) return false;
    constexpr std::size_t minimum_entry_size = 40;
    if (count > (bytes.size() - cursor) / minimum_entry_size ||
        count > staged.max_size() - staged.size() ||
        (count != 0 && count - 1 > UINT64_MAX - first)) return false;
    staged.reserve(staged.size() + static_cast<std::size_t>(count));
    for (std::uint64_t n = 0; n < count; ++n) {
      CrudIndexEntryRecord entry;
      entry.creator_tx = creator;
      entry.event_sequence = first + n;
      entry.sequence = entry.event_sequence;
      entry.table_uuid = table;
      entry.index_uuid = index;
      entry.column_name = column;
      entry.family = family;
      entry.entry_kind = kind;
      if (!ReadBinaryString(bytes, &cursor, &entry.key_value) || entry.key_value.empty() ||
          !ReadBinaryString(bytes, &cursor, &entry.payload_value) ||
          !ReadBinaryEngineUuid(bytes, &cursor, &entry.row_uuid) ||
          !ReadBinaryEngineUuid(bytes, &cursor, &entry.version_uuid)) return false;
      staged.push_back(std::move(entry));
    }
  }
  if (staged.size() > entries->max_size() - entries->size()) return false;
  entries->insert(entries->end(), std::make_move_iterator(staged.begin()),
                  std::make_move_iterator(staged.end()));
  return true;
}
}  // namespace scratchbird::engine::internal_api
