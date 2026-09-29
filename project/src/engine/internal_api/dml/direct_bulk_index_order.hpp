// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {
// Transient index preparation only. No catalog, transaction or publication authority.
struct DirectPrecomputedIndexEntry {
  std::string encoded_key;
  std::string payload_value;
  EngineUuid row_uuid;
  EngineUuid version_uuid;
  std::uint64_t source_ordinal = 0;
  bool null_key = false;
};

using DirectPrecomputedIndexEntryMap =
    std::map<EngineUuid, std::vector<DirectPrecomputedIndexEntry>>;

struct DirectPrecomputedIndexEntryOrderState {
  bool initialized = false;
  bool append_order_sorted = true;
  bool duplicate_keys_absent = true;
  DirectPrecomputedIndexEntry last_entry;
};

using DirectPrecomputedIndexEntryOrderStateMap =
    std::map<EngineUuid, DirectPrecomputedIndexEntryOrderState>;

bool DirectPrecomputedIndexEntryLess(const DirectPrecomputedIndexEntry&, const DirectPrecomputedIndexEntry&);
void DirectTrackPrecomputedIndexEntryOrder(const EngineUuid&, const DirectPrecomputedIndexEntry&, DirectPrecomputedIndexEntryOrderStateMap*);
bool DirectPrecomputedIndexEntriesAppendOrderSorted(const DirectPrecomputedIndexEntryMap&, const DirectPrecomputedIndexEntryOrderStateMap&);
bool DirectPrecomputedIndexEntriesDuplicateKeysAbsent(const DirectPrecomputedIndexEntryMap&, const DirectPrecomputedIndexEntryOrderStateMap&);
bool DirectPrecomputedIndexEntryTextLess(const DirectPrecomputedIndexEntry&, const DirectPrecomputedIndexEntry&);
bool DirectPrecomputedIndexEntryExactAppendLess(const DirectPrecomputedIndexEntry&, const DirectPrecomputedIndexEntry&);
bool DirectPrecomputedEntriesRequireEncodedCompare(const std::vector<DirectPrecomputedIndexEntry>&);
void DirectSortPrecomputedIndexEntries(DirectPrecomputedIndexEntryMap*);
void DirectSortPrecomputedIndexEntriesForExactAppend(std::vector<DirectPrecomputedIndexEntry>*);
const std::vector<DirectPrecomputedIndexEntry>* DirectPrecomputedEntriesForIndex(const DirectPrecomputedIndexEntryMap*, const EngineUuid&);
bool DirectPrecomputedEntriesHaveDuplicateKeys(const std::vector<DirectPrecomputedIndexEntry>&);
}  // namespace scratchbird::engine::internal_api::dml::detail
