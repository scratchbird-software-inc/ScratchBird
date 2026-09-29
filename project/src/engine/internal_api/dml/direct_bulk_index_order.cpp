// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "direct_bulk_index_order.hpp"
#include "direct_binary_scalar_index_key.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <numeric>
#include <utility>

namespace scratchbird::engine::internal_api::dml::detail {
// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_INDEX_ORDER_AUTHORITY
// Order retained keys with binary UUID tie-breaks; keep payloads paired with rows.
bool DirectPrecomputedIndexEntryLess(
    const DirectPrecomputedIndexEntry& left,
    const DirectPrecomputedIndexEntry& right) {
  const int key_compare =
      DirectCompareEncodedIndexKey(left.encoded_key, right.encoded_key);
  if (key_compare != 0) {
    return key_compare < 0;
  }
  const int row_compare =
      scratchbird::core::uuid::CompareUuid128(left.row_uuid, right.row_uuid);
  if (row_compare != 0) {
    return row_compare < 0;
  }
  const int version_compare =
      scratchbird::core::uuid::CompareUuid128(left.version_uuid, right.version_uuid);
  if (version_compare != 0) {
    return version_compare < 0;
  }
  return left.source_ordinal < right.source_ordinal;
}

void DirectTrackPrecomputedIndexEntryOrder(
    const EngineUuid& index_uuid,
    const DirectPrecomputedIndexEntry& entry,
    DirectPrecomputedIndexEntryOrderStateMap* states) {
  if (states == nullptr) {
    return;
  }
  auto& state = (*states)[index_uuid];
  if (!state.initialized) {
    state.initialized = true;
    state.last_entry = entry;
    return;
  }
  if (DirectPrecomputedIndexEntryLess(entry, state.last_entry)) {
    state.append_order_sorted = false;
  }
  if (DirectCompareEncodedIndexKey(entry.encoded_key,
                                   state.last_entry.encoded_key) == 0) {
    state.duplicate_keys_absent = false;
  }
  state.last_entry = entry;
}

bool DirectPrecomputedIndexEntriesAppendOrderSorted(
    const DirectPrecomputedIndexEntryMap& entries_by_index,
    const DirectPrecomputedIndexEntryOrderStateMap& states) {
  for (const auto& [index_uuid, entries] : entries_by_index) {
    if (entries.size() <= 1) {
      continue;
    }
    const auto found = states.find(index_uuid);
    if (found == states.end() || !found->second.initialized ||
        !found->second.append_order_sorted) {
      return false;
    }
  }
  return true;
}

bool DirectPrecomputedIndexEntriesDuplicateKeysAbsent(
    const DirectPrecomputedIndexEntryMap& entries_by_index,
    const DirectPrecomputedIndexEntryOrderStateMap& states) {
  for (const auto& [index_uuid, entries] : entries_by_index) {
    if (entries.size() <= 1) {
      continue;
    }
    const auto found = states.find(index_uuid);
    if (found == states.end() || !found->second.initialized ||
        !found->second.duplicate_keys_absent) {
      return false;
    }
  }
  return true;
}

bool DirectPrecomputedIndexEntryTextLess(
    const DirectPrecomputedIndexEntry& left,
    const DirectPrecomputedIndexEntry& right) {
  if (left.encoded_key != right.encoded_key) {
    return left.encoded_key < right.encoded_key;
  }
  if (left.row_uuid != right.row_uuid) {
    return left.row_uuid < right.row_uuid;
  }
  if (left.version_uuid != right.version_uuid) {
    return left.version_uuid < right.version_uuid;
  }
  return left.source_ordinal < right.source_ordinal;
}

bool DirectPrecomputedIndexEntryExactAppendLess(
    const DirectPrecomputedIndexEntry& left,
    const DirectPrecomputedIndexEntry& right) {
  if (left.encoded_key != right.encoded_key) {
    return left.encoded_key < right.encoded_key;
  }
  if (left.row_uuid != right.row_uuid) {
    return left.row_uuid < right.row_uuid;
  }
  if (left.version_uuid != right.version_uuid) {
    return left.version_uuid < right.version_uuid;
  }
  return left.source_ordinal < right.source_ordinal;
}

bool DirectPrecomputedEntriesRequireEncodedCompare(
    const std::vector<DirectPrecomputedIndexEntry>& entries) {
  return std::all_of(entries.begin(),
                     entries.end(),
                     [](const auto& entry) {
                       return scratchbird::core::index::
                                  IsOrderPreservingIndexKeyEncoding(
                                      entry.encoded_key) ||
                              entry.encoded_key.rfind(
                                  kDirectSbkoBinaryPrefix, 0) == 0;
                     });
}

void DirectSortPrecomputedIndexEntries(DirectPrecomputedIndexEntryMap* map) {
  if (map == nullptr) {
    return;
  }
  for (auto& [unused_index_uuid, entries] : *map) {
    (void)unused_index_uuid;
    if (entries.size() <= 1) {
      continue;
    }
    const bool use_encoded_compare =
        DirectPrecomputedEntriesRequireEncodedCompare(entries);
    const auto entry_less = [&](std::size_t left, std::size_t right) {
      return use_encoded_compare
                 ? DirectPrecomputedIndexEntryLess(entries[left],
                                                   entries[right])
                 : DirectPrecomputedIndexEntryTextLess(entries[left],
                                                       entries[right]);
    };
    const auto direct_less = [&](const auto& left, const auto& right) {
      return use_encoded_compare ? DirectPrecomputedIndexEntryLess(left, right)
                                 : DirectPrecomputedIndexEntryTextLess(left, right);
    };
    if (std::is_sorted(entries.begin(), entries.end(), direct_less)) {
      continue;
    }
    std::vector<std::size_t> order(entries.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), entry_less);
    std::vector<DirectPrecomputedIndexEntry> sorted;
    sorted.reserve(entries.size());
    for (const auto ordinal : order) {
      sorted.push_back(std::move(entries[ordinal]));
    }
    entries = std::move(sorted);
  }
}

void DirectSortPrecomputedIndexEntriesForExactAppend(
    std::vector<DirectPrecomputedIndexEntry>* entries) {
  if (entries == nullptr || entries->size() <= 1) {
    return;
  }
  if (std::is_sorted(entries->begin(),
                     entries->end(),
                     DirectPrecomputedIndexEntryExactAppendLess)) {
    return;
  }
  std::stable_sort(entries->begin(),
                   entries->end(),
                   DirectPrecomputedIndexEntryExactAppendLess);
}

const std::vector<DirectPrecomputedIndexEntry>* DirectPrecomputedEntriesForIndex(
    const DirectPrecomputedIndexEntryMap* precomputed_entries,
    const EngineUuid& index_uuid) {
  if (precomputed_entries == nullptr) {
    return nullptr;
  }
  const auto found = precomputed_entries->find(index_uuid);
  return found == precomputed_entries->end() ? nullptr : &found->second;
}

bool DirectPrecomputedEntriesHaveDuplicateKeys(
    const std::vector<DirectPrecomputedIndexEntry>& entries) {
  if (entries.size() < 2) {
    return false;
  }
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (DirectCompareEncodedIndexKey(entries[index - 1].encoded_key,
                                     entries[index].encoded_key) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace scratchbird::engine::internal_api::dml::detail
