// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hot_point_lookup_cache.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

constexpr const char* kDiagHit = "SB_INDEX_HOT_POINT_LOOKUP_CACHE_HIT";
constexpr const char* kDiagMiss = "SB_INDEX_HOT_POINT_LOOKUP_CACHE_MISS";
constexpr const char* kDiagStaleEpoch = "SB_INDEX_HOT_POINT_LOOKUP_CACHE_STALE_EPOCH";
constexpr const char* kDiagDependencyInvalidated =
    "SB_INDEX_HOT_POINT_LOOKUP_CACHE_DEPENDENCY_INVALIDATED";
constexpr const char* kDiagAuthorityRefused =
    "SB_INDEX_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED";
constexpr const char* kDiagContentionDisabled =
    "SB_INDEX_HOT_POINT_LOOKUP_CACHE_PARTITION_AUTO_DISABLED";
constexpr const char* kDiagContentionRefused =
    "SB_INDEX_HOT_POINT_LOOKUP_CACHE_CONTENTION_REFUSED";
constexpr const char* kDiagAdmitted = "SB_INDEX_HOT_POINT_LOOKUP_CACHE_ADMITTED";
constexpr const char* kDiagReset = "SB_INDEX_HOT_POINT_LOOKUP_CACHE_PARTITION_RESET";

std::string UuidKey(const TypedUuid& uuid) {
  if (!uuid.valid()) {
    return "invalid";
  }
  std::ostringstream out;
  out << static_cast<unsigned>(uuid.kind) << ':';
  out << std::hex << std::setfill('0');
  for (auto value : uuid.value.bytes) {
    out << std::setw(2) << static_cast<unsigned>(value);
  }
  return out.str();
}

void AppendUuidVector(std::ostringstream& out,
                      const char* label,
                      std::vector<TypedUuid> uuids) {
  std::vector<std::string> keys;
  keys.reserve(uuids.size());
  for (const auto& uuid : uuids) {
    keys.push_back(UuidKey(uuid));
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  out << '|' << label << '=';
  for (const auto& key : keys) {
    out << key << ';';
  }
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool UuidMatchesIfPresent(const TypedUuid& event_uuid, const TypedUuid& entry_uuid) {
  return !event_uuid.valid() || SameUuid(event_uuid, entry_uuid);
}

bool DependencyMatches(const HotPointLookupCacheEntry& entry,
                       const TypedUuid& dependency_uuid) {
  if (!dependency_uuid.valid()) {
    return true;
  }
  if (SameUuid(entry.key.object_uuid, dependency_uuid) ||
      SameUuid(entry.key.index_uuid, dependency_uuid)) {
    return true;
  }
  for (const auto& uuid : entry.dependency_uuids) {
    if (SameUuid(uuid, dependency_uuid)) {
      return true;
    }
  }
  return false;
}

bool EventEpochInvalidates(const HotPointLookupCacheEntry& entry,
                           const HotPointLookupInvalidationEvent& event) {
  return (event.catalog_epoch != 0 && entry.key.catalog_epoch < event.catalog_epoch) ||
         (event.index_epoch != 0 && entry.key.index_epoch < event.index_epoch) ||
         (event.statistics_epoch != 0 &&
          entry.key.statistics_epoch < event.statistics_epoch) ||
         (event.security_epoch != 0 && entry.key.security_epoch < event.security_epoch) ||
         (event.policy_epoch != 0 && entry.key.policy_epoch < event.policy_epoch) ||
         (event.object_epoch != 0 && entry.key.object_epoch < event.object_epoch);
}

bool EventInvalidatesEntry(const HotPointLookupCacheEntry& entry,
                           const HotPointLookupInvalidationEvent& event) {
  if (!HotPointLookupInvalidationEventRecognized(event)) {
    return true;
  }
  if (!UuidMatchesIfPresent(event.object_uuid, entry.key.object_uuid) ||
      !UuidMatchesIfPresent(event.index_uuid, entry.key.index_uuid)) {
    return false;
  }
  if (EventEpochInvalidates(entry, event)) {
    return true;
  }
  return DependencyMatches(entry, event.dependency_uuid);
}

void AddReuseEvidence(HotPointLookupCacheResult* result,
                      const HotPointLookupCacheEntry& entry) {
  result->evidence.push_back("candidate_locator_only=true");
  result->evidence.push_back("mga_visibility_recheck=required");
  result->evidence.push_back("security_authorization_recheck=required");
  result->evidence.push_back("cache_visibility_finality_authority=false");
  result->evidence.push_back("engine_mga_recheck_required_after_hit=true");
  result->evidence.push_back("probe_class=" +
                             std::string(HotPointProbeClassName(entry.key.probe_class)));
}

void AddPartitionEvidence(HotPointLookupCacheResult* result) {
  result->evidence.push_back("partition=" + std::to_string(result->partition));
}

}  // namespace

struct AdaptiveHotPointLookupCache::Partition {
  mutable std::mutex mutex;
  std::map<std::string, HotPointLookupCacheEntry> entries;
  HotPointLookupPartitionCounters counters;
  std::atomic<u64> contention_refusals{0};
  std::atomic<bool> auto_disabled{false};
};

AdaptiveHotPointLookupCache::AdaptiveHotPointLookupCache(
    HotPointLookupCacheConfig config)
    : config_(std::move(config)) {
  if (config_.partition_count == 0) {
    config_.partition_count = 1;
  }
  if (config_.contention_disable_threshold == 0) {
    config_.contention_disable_threshold = 1;
  }
  if (config_.authority_refusal_disable_threshold == 0) {
    config_.authority_refusal_disable_threshold = 1;
  }
  partitions_.reserve(config_.partition_count);
  for (std::size_t i = 0; i < config_.partition_count; ++i) {
    partitions_.push_back(std::make_unique<Partition>());
  }
}

AdaptiveHotPointLookupCache::~AdaptiveHotPointLookupCache() = default;

HotPointLookupCacheResult AdaptiveHotPointLookupCache::Put(
    HotPointLookupCacheEntry entry) {
  HotPointLookupCacheResult result;
  result.cache_key = BuildHotPointLookupCacheKey(entry.key);
  result.partition = PartitionForKey(entry.key);
  AddPartitionEvidence(&result);
  auto& partition = *partitions_[result.partition];

  if (partition.auto_disabled.load(std::memory_order_relaxed)) {
    result.diagnostic_code = kDiagContentionDisabled;
    result.evidence.push_back("admission_failed_open=partition_auto_disabled");
    if (partition.mutex.try_lock()) {
      std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);
      ++partition.counters.disabled_admission_refusals;
    }
    return result;
  }
  if (!partition.mutex.try_lock()) {
    const auto refusals =
        partition.contention_refusals.fetch_add(1, std::memory_order_relaxed) + 1;
    if (refusals >= config_.contention_disable_threshold) {
      partition.auto_disabled.store(true, std::memory_order_relaxed);
    }
    result.diagnostic_code = kDiagContentionRefused;
    result.evidence.push_back("admission_failed_open=partition_contention");
    return result;
  }

  std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);
  if (!HotPointLookupEntrySafeForCache(entry)) {
    ++partition.counters.authority_refusals;
    if (partition.counters.authority_refusals >=
        config_.authority_refusal_disable_threshold) {
      partition.auto_disabled.store(true, std::memory_order_relaxed);
    }
    result.diagnostic_code = kDiagAuthorityRefused;
    result.evidence.push_back("authority_refusal=candidate_recheck_required");
    result.evidence.push_back("cache_visibility_finality_authority=false");
    result.evidence.push_back("admission_failed_open=authority_refused");
    return result;
  }

  if (config_.max_entries_per_partition != 0 &&
      partition.entries.size() >= config_.max_entries_per_partition) {
    partition.entries.erase(partition.entries.begin());
  }
  entry.valid = true;
  entry.invalidated_by_dependency = false;
  entry.invalidation_diagnostic_code.clear();
  entry.invalidation_event_kind.clear();
  entry.invalidation_dependency_uuid.clear();
  partition.entries[result.cache_key] = std::move(entry);
  ++partition.counters.puts;
  partition.counters.entry_count = partition.entries.size();
  result.ok = true;
  result.admitted = true;
  result.diagnostic_code = kDiagAdmitted;
  result.evidence.push_back("hot_point_lookup_cache_admitted");
  result.evidence.push_back("admission_metadata_only=true");
  return result;
}

HotPointLookupCacheResult AdaptiveHotPointLookupCache::Lookup(
    const HotPointLookupCacheKey& key) {
  HotPointLookupCacheResult result;
  result.cache_key = BuildHotPointLookupCacheKey(key);
  result.partition = PartitionForKey(key);
  AddPartitionEvidence(&result);
  auto& partition = *partitions_[result.partition];

  if (partition.auto_disabled.load(std::memory_order_relaxed)) {
    if (partition.mutex.try_lock()) {
      std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);
      ++partition.counters.disabled_lookup_refusals;
    }
    result.diagnostic_code = kDiagContentionDisabled;
    result.evidence.push_back("lookup_failed_open=partition_auto_disabled");
    return result;
  }
  if (!partition.mutex.try_lock()) {
    const auto refusals =
        partition.contention_refusals.fetch_add(1, std::memory_order_relaxed) + 1;
    if (refusals >= config_.contention_disable_threshold) {
      partition.auto_disabled.store(true, std::memory_order_relaxed);
    }
    result.diagnostic_code = kDiagContentionRefused;
    result.evidence.push_back("lookup_failed_open=partition_contention");
    return result;
  }

  std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);
  auto exact = partition.entries.find(result.cache_key);
  if (exact != partition.entries.end()) {
    result.entry = exact->second;
    if (!exact->second.valid) {
      ++partition.counters.misses;
      result.diagnostic_code = exact->second.invalidation_diagnostic_code.empty()
                                   ? kDiagDependencyInvalidated
                                   : exact->second.invalidation_diagnostic_code;
      result.evidence.push_back("hot_point_lookup_cache_dependency_invalidation");
      if (!exact->second.invalidation_event_kind.empty()) {
        result.evidence.push_back("invalidation_kind=" +
                                  exact->second.invalidation_event_kind);
      }
      if (!exact->second.invalidation_dependency_uuid.empty()) {
        result.evidence.push_back("invalidation_dependency=" +
                                  exact->second.invalidation_dependency_uuid);
      }
      return result;
    }
    if (!HotPointLookupEntrySafeForCache(exact->second)) {
      ++partition.counters.authority_refusals;
      if (partition.counters.authority_refusals >=
          config_.authority_refusal_disable_threshold) {
        partition.auto_disabled.store(true, std::memory_order_relaxed);
      }
      result.diagnostic_code = kDiagAuthorityRefused;
      result.evidence.push_back("lookup_failed_open=authority_refused");
      result.evidence.push_back("cache_visibility_finality_authority=false");
      return result;
    }
    ++partition.counters.hits;
    result.ok = true;
    result.cache_hit = true;
    result.diagnostic_code = kDiagHit;
    result.evidence.push_back("hot_point_lookup_cache_hit");
    AddReuseEvidence(&result, exact->second);
    return result;
  }

  const auto stable_key = BuildHotPointLookupStableProbeKey(key);
  for (const auto& [cached_key, cached_entry] : partition.entries) {
    (void)cached_key;
    if (BuildHotPointLookupStableProbeKey(cached_entry.key) != stable_key) {
      continue;
    }
    result.entry = cached_entry;
    if (!cached_entry.valid) {
      ++partition.counters.misses;
      result.diagnostic_code = cached_entry.invalidation_diagnostic_code.empty()
                                   ? kDiagDependencyInvalidated
                                   : cached_entry.invalidation_diagnostic_code;
      result.evidence.push_back("hot_point_lookup_cache_dependency_invalidation");
      return result;
    }
    if (!HotPointLookupKeyEpochCompatible(cached_entry.key, key)) {
      ++partition.counters.stale_epoch_refusals;
      result.diagnostic_code = kDiagStaleEpoch;
      result.evidence.push_back("hot_point_lookup_cache_stale_epoch");
      result.evidence.push_back("epoch_compatibility=false");
      return result;
    }
  }

  ++partition.counters.misses;
  result.diagnostic_code = kDiagMiss;
  result.evidence.push_back("hot_point_lookup_cache_miss");
  return result;
}

HotPointLookupCacheResult AdaptiveHotPointLookupCache::RecordContentionRefusal(
    const HotPointLookupCacheKey& key) {
  HotPointLookupCacheResult result;
  result.cache_key = BuildHotPointLookupCacheKey(key);
  result.partition = PartitionForKey(key);
  AddPartitionEvidence(&result);
  auto& partition = *partitions_[result.partition];
  const auto refusals =
      partition.contention_refusals.fetch_add(1, std::memory_order_relaxed) + 1;
  if (refusals >= config_.contention_disable_threshold) {
    partition.auto_disabled.store(true, std::memory_order_relaxed);
  }
  result.diagnostic_code = partition.auto_disabled.load(std::memory_order_relaxed)
                               ? kDiagContentionDisabled
                               : kDiagContentionRefused;
  result.evidence.push_back("lookup_failed_open=partition_contention");
  result.evidence.push_back("contention_refusals=" + std::to_string(refusals));
  return result;
}

HotPointLookupInvalidationResult AdaptiveHotPointLookupCache::Invalidate(
    const HotPointLookupInvalidationEvent& event) {
  HotPointLookupInvalidationResult result;
  result.diagnostic_code = HotPointLookupInvalidationEventRecognized(event)
                               ? kDiagDependencyInvalidated
                               : "SB_INDEX_HOT_POINT_LOOKUP_CACHE_UNKNOWN_INVALIDATION";
  result.evidence.push_back("hot_point_lookup_cache_invalidation");
  result.evidence.push_back("invalidation_kind=" + event.event_kind);
  if (event.dependency_uuid.valid()) {
    result.evidence.push_back("dependency_uuid=" + UuidKey(event.dependency_uuid));
  }
  if (event.object_uuid.valid()) {
    result.evidence.push_back("object_uuid=" + UuidKey(event.object_uuid));
  }
  if (event.index_uuid.valid()) {
    result.evidence.push_back("index_uuid=" + UuidKey(event.index_uuid));
  }

  for (auto& partition_ptr : partitions_) {
    auto& partition = *partition_ptr;
    std::lock_guard<std::mutex> lock(partition.mutex);
    for (auto& [cache_key, entry] : partition.entries) {
      (void)cache_key;
      if (entry.valid && EventInvalidatesEntry(entry, event)) {
        entry.valid = false;
        entry.invalidated_by_dependency = true;
        entry.invalidation_diagnostic_code = result.diagnostic_code;
        entry.invalidation_event_kind = event.event_kind;
        entry.invalidation_dependency_uuid = event.dependency_uuid.valid()
                                                 ? UuidKey(event.dependency_uuid)
                                                 : UuidKey(event.object_uuid.valid()
                                                               ? event.object_uuid
                                                               : event.index_uuid);
        ++partition.counters.dependency_invalidations;
        ++result.invalidated_count;
      }
    }
  }
  result.evidence.push_back("invalidated_count=" +
                            std::to_string(result.invalidated_count));
  return result;
}

HotPointLookupCacheResult AdaptiveHotPointLookupCache::ResetPartition(
    std::size_t partition_index) {
  HotPointLookupCacheResult result;
  if (partition_index >= partitions_.size()) {
    partition_index = 0;
  }
  result.partition = partition_index;
  result.diagnostic_code = kDiagReset;
  AddPartitionEvidence(&result);
  auto& partition = *partitions_[partition_index];
  std::lock_guard<std::mutex> lock(partition.mutex);
  partition.entries.clear();
  partition.counters = {};
  partition.counters.resets = 1;
  partition.contention_refusals.store(0, std::memory_order_relaxed);
  partition.auto_disabled.store(false, std::memory_order_relaxed);
  result.ok = true;
  result.evidence.push_back("partition_reset=true");
  result.evidence.push_back("lookup_cache_reenabled=true");
  return result;
}

void AdaptiveHotPointLookupCache::Clear() {
  for (auto& partition_ptr : partitions_) {
    auto& partition = *partition_ptr;
    std::lock_guard<std::mutex> lock(partition.mutex);
    partition.entries.clear();
    partition.counters.entry_count = 0;
  }
}

std::size_t AdaptiveHotPointLookupCache::PartitionForKey(
    const HotPointLookupCacheKey& key) const {
  return std::hash<std::string>{}(BuildHotPointLookupStableProbeKey(key)) %
         partitions_.size();
}

HotPointLookupPartitionCounters AdaptiveHotPointLookupCache::PartitionCounters(
    std::size_t partition_index) const {
  if (partition_index >= partitions_.size()) {
    partition_index = 0;
  }
  const auto& partition = *partitions_[partition_index];
  std::lock_guard<std::mutex> lock(partition.mutex);
  auto counters = partition.counters;
  counters.contention_refusals =
      partition.contention_refusals.load(std::memory_order_relaxed);
  counters.auto_disabled = partition.auto_disabled.load(std::memory_order_relaxed);
  counters.entry_count = partition.entries.size();
  return counters;
}

std::vector<HotPointLookupPartitionCounters>
AdaptiveHotPointLookupCache::SnapshotCounters() const {
  std::vector<HotPointLookupPartitionCounters> snapshot;
  snapshot.reserve(partitions_.size());
  for (std::size_t i = 0; i < partitions_.size(); ++i) {
    snapshot.push_back(PartitionCounters(i));
  }
  return snapshot;
}

const char* HotPointProbeClassName(HotPointProbeClass probe_class) {
  switch (probe_class) {
    case HotPointProbeClass::row_uuid_lookup:
      return "row_uuid_lookup";
    case HotPointProbeClass::unique_index_lookup:
      return "unique_index_lookup";
    case HotPointProbeClass::fk_parent_existence_lookup:
      return "fk_parent_existence_lookup";
    case HotPointProbeClass::conflict_preflight_lookup:
      return "conflict_preflight_lookup";
  }
  return "unknown";
}

std::string BuildHotPointLookupCacheKey(const HotPointLookupCacheKey& key) {
  std::ostringstream out;
  out << BuildHotPointLookupStableProbeKey(key)
      << "|stats_snapshot=" << key.statistics_snapshot_id
      << "|descriptor_set=" << key.descriptor_set_digest
      << "|index_definition=" << key.index_definition_digest
      << "|security_policy=" << key.security_policy_digest
      << "|redaction_policy=" << key.redaction_policy_digest
      << "|access_policy=" << key.access_policy_digest
      << "|collation_profile=" << key.collation_profile_digest
      << "|catalog_epoch=" << key.catalog_epoch
      << "|index_epoch=" << key.index_epoch
      << "|statistics_epoch=" << key.statistics_epoch
      << "|security_epoch=" << key.security_epoch
      << "|policy_epoch=" << key.policy_epoch
      << "|object_epoch=" << key.object_epoch
      << "|compatibility_epoch=" << key.compatibility_epoch;
  return out.str();
}

std::string BuildHotPointLookupStableProbeKey(const HotPointLookupCacheKey& key) {
  std::ostringstream out;
  out << "probe_class=" << HotPointProbeClassName(key.probe_class)
      << "|database_uuid=" << UuidKey(key.database_uuid)
      << "|object_uuid=" << UuidKey(key.object_uuid)
      << "|index_uuid=" << UuidKey(key.index_uuid)
      << "|encoded_probe_key=" << key.encoded_probe_key;
  return out.str();
}

bool HotPointLookupKeyEpochCompatible(const HotPointLookupCacheKey& cached,
                                      const HotPointLookupCacheKey& requested) {
  return cached.catalog_epoch == requested.catalog_epoch &&
         cached.index_epoch == requested.index_epoch &&
         cached.statistics_epoch == requested.statistics_epoch &&
         cached.security_epoch == requested.security_epoch &&
         cached.policy_epoch == requested.policy_epoch &&
         cached.object_epoch == requested.object_epoch &&
         cached.compatibility_epoch == requested.compatibility_epoch &&
         cached.statistics_snapshot_id == requested.statistics_snapshot_id &&
         cached.descriptor_set_digest == requested.descriptor_set_digest &&
         cached.index_definition_digest == requested.index_definition_digest &&
         cached.security_policy_digest == requested.security_policy_digest &&
         cached.redaction_policy_digest == requested.redaction_policy_digest &&
         cached.access_policy_digest == requested.access_policy_digest &&
         cached.collation_profile_digest == requested.collation_profile_digest;
}

bool HotPointLookupCandidateSafeForCache(
    const HotPointLookupCandidate& candidate) {
  return candidate.candidate_locator_only &&
         candidate.equality_proof_metadata_only &&
         candidate.locator.table_uuid.valid() &&
         candidate.locator.row_uuid.valid() &&
         candidate.requires_mga_visibility_recheck &&
         candidate.requires_security_authorization_recheck &&
         !candidate.visibility_finality_authority &&
         !candidate.authorization_finality_authority &&
         !candidate.parser_or_reference_finality_authority &&
         !candidate.timestamp_or_uuid_order_finality_authority;
}

bool HotPointLookupEntrySafeForCache(const HotPointLookupCacheEntry& entry) {
  if (entry.candidates.empty()) {
    return false;
  }
  for (const auto& candidate : entry.candidates) {
    if (!HotPointLookupCandidateSafeForCache(candidate)) {
      return false;
    }
  }
  return true;
}

bool HotPointLookupInvalidationEventRecognized(
    const HotPointLookupInvalidationEvent& event) {
  return event.event_kind == "catalog_epoch" ||
         event.event_kind == "index_epoch" ||
         event.event_kind == "statistics_epoch" ||
         event.event_kind == "security_epoch" ||
         event.event_kind == "policy_epoch" ||
         event.event_kind == "object_epoch" ||
         event.event_kind == "catalog_alter" ||
         event.event_kind == "catalog_drop" ||
         event.event_kind == "index_change" ||
         event.event_kind == "statistics_refresh" ||
         event.event_kind == "security_policy_change" ||
         event.event_kind == "redaction_policy_change" ||
         event.event_kind == "access_policy_change" ||
         event.event_kind == "nosql_generation_publish" ||
         event.event_kind == "nosql_generation_retire" ||
         event.event_kind == "nosql_compaction" ||
         event.event_kind == "nosql_family_compaction" ||
         event.event_kind == "dependency_invalidation";
}

}  // namespace scratchbird::core::index
