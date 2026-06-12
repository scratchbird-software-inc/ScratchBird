// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-HOT-POINT-LOOKUP-CACHE-CLOSURE-ANCHOR

#include "index_access_method.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class HotPointProbeClass : u32 {
  row_uuid_lookup = 1,
  unique_index_lookup = 2,
  fk_parent_existence_lookup = 3,
  conflict_preflight_lookup = 4
};

struct HotPointLookupCacheConfig {
  std::size_t partition_count = 16;
  u64 contention_disable_threshold = 64;
  u64 authority_refusal_disable_threshold = 64;
  u64 max_entries_per_partition = 1024;
};

struct HotPointLookupCacheKey {
  HotPointProbeClass probe_class = HotPointProbeClass::row_uuid_lookup;
  TypedUuid database_uuid;
  TypedUuid object_uuid;
  TypedUuid index_uuid;
  std::string encoded_probe_key;
  std::string statistics_snapshot_id;
  std::string descriptor_set_digest;
  std::string index_definition_digest;
  std::string security_policy_digest;
  std::string redaction_policy_digest;
  std::string access_policy_digest;
  std::string collation_profile_digest;
  u64 catalog_epoch = 0;
  u64 index_epoch = 0;
  u64 statistics_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  u64 object_epoch = 0;
  u64 compatibility_epoch = 0;
};

struct HotPointLookupCandidate {
  IndexRowLocator locator;
  std::string proof_kind = "candidate_locator";
  std::string posting_list_digest;
  bool candidate_locator_only = true;
  bool equality_proof_metadata_only = true;
  bool requires_mga_visibility_recheck = true;
  bool requires_security_authorization_recheck = true;
  bool visibility_finality_authority = false;
  bool authorization_finality_authority = false;
  bool parser_or_reference_finality_authority = false;
  bool timestamp_or_uuid_order_finality_authority = false;
};

struct HotPointLookupCacheEntry {
  HotPointLookupCacheKey key;
  std::vector<HotPointLookupCandidate> candidates;
  std::vector<TypedUuid> dependency_uuids;
  u64 created_epoch = 0;
  bool valid = true;
  bool invalidated_by_dependency = false;
  std::string invalidation_diagnostic_code;
  std::string invalidation_event_kind;
  std::string invalidation_dependency_uuid;
};

struct HotPointLookupInvalidationEvent {
  std::string event_kind;
  TypedUuid dependency_uuid;
  TypedUuid object_uuid;
  TypedUuid index_uuid;
  u64 catalog_epoch = 0;
  u64 index_epoch = 0;
  u64 statistics_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  u64 object_epoch = 0;
};

struct HotPointLookupPartitionCounters {
  u64 puts = 0;
  u64 hits = 0;
  u64 misses = 0;
  u64 stale_epoch_refusals = 0;
  u64 dependency_invalidations = 0;
  u64 authority_refusals = 0;
  u64 contention_refusals = 0;
  u64 disabled_lookup_refusals = 0;
  u64 disabled_admission_refusals = 0;
  u64 resets = 0;
  bool auto_disabled = false;
  std::size_t entry_count = 0;
};

struct HotPointLookupCacheResult {
  bool ok = false;
  bool cache_hit = false;
  bool admitted = false;
  std::size_t partition = 0;
  std::string cache_key;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::optional<HotPointLookupCacheEntry> entry;
};

struct HotPointLookupInvalidationResult {
  u64 invalidated_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

class AdaptiveHotPointLookupCache {
 public:
  explicit AdaptiveHotPointLookupCache(HotPointLookupCacheConfig config = {});
  ~AdaptiveHotPointLookupCache();

  AdaptiveHotPointLookupCache(const AdaptiveHotPointLookupCache&) = delete;
  AdaptiveHotPointLookupCache& operator=(const AdaptiveHotPointLookupCache&) = delete;

  HotPointLookupCacheResult Put(HotPointLookupCacheEntry entry);
  HotPointLookupCacheResult Lookup(const HotPointLookupCacheKey& key);
  HotPointLookupCacheResult RecordContentionRefusal(
      const HotPointLookupCacheKey& key);
  HotPointLookupInvalidationResult Invalidate(
      const HotPointLookupInvalidationEvent& event);
  HotPointLookupCacheResult ResetPartition(std::size_t partition);
  void Clear();

  std::size_t PartitionForKey(const HotPointLookupCacheKey& key) const;
  HotPointLookupPartitionCounters PartitionCounters(std::size_t partition) const;
  std::vector<HotPointLookupPartitionCounters> SnapshotCounters() const;

 private:
  struct Partition;

  HotPointLookupCacheConfig config_;
  std::vector<std::unique_ptr<Partition>> partitions_;
};

const char* HotPointProbeClassName(HotPointProbeClass probe_class);
std::string BuildHotPointLookupCacheKey(const HotPointLookupCacheKey& key);
std::string BuildHotPointLookupStableProbeKey(const HotPointLookupCacheKey& key);
bool HotPointLookupKeyEpochCompatible(const HotPointLookupCacheKey& cached,
                                      const HotPointLookupCacheKey& requested);
bool HotPointLookupCandidateSafeForCache(const HotPointLookupCandidate& candidate);
bool HotPointLookupEntrySafeForCache(const HotPointLookupCacheEntry& entry);
bool HotPointLookupInvalidationEventRecognized(
    const HotPointLookupInvalidationEvent& event);

}  // namespace scratchbird::core::index
