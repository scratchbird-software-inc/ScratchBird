// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_CATALOG_PINNED_DESCRIPTOR_CACHE_ODF_021
// Epoch-pinned descriptor snapshots are read-only hot-path metadata. They do
// not cache MGA visibility, authorization finality, or mutable catalog state.

struct CatalogPinnedDescriptorCacheKey {
  std::string descriptor_family = "catalog_descriptor";
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_policy_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t stats_epoch = 0;
  bool stats_epoch_relevant = false;
  std::string descriptor_set_digest;
  std::vector<EngineUuid> object_uuids;
  std::vector<EngineUuid> index_uuids;
  std::string security_policy_identity;
  std::string redaction_policy_identity;
  std::string resource_policy_identity;

  bool operator==(const CatalogPinnedDescriptorCacheKey&) const = default;
  bool operator<(const CatalogPinnedDescriptorCacheKey& other) const {
    return std::tie(descriptor_family, catalog_epoch, security_epoch,
                    resource_policy_epoch, name_resolution_epoch, stats_epoch,
                    stats_epoch_relevant, descriptor_set_digest, object_uuids,
                    index_uuids, security_policy_identity,
                    redaction_policy_identity, resource_policy_identity) <
           std::tie(other.descriptor_family, other.catalog_epoch, other.security_epoch,
                    other.resource_policy_epoch, other.name_resolution_epoch, other.stats_epoch,
                    other.stats_epoch_relevant, other.descriptor_set_digest, other.object_uuids,
                    other.index_uuids, other.security_policy_identity,
                    other.redaction_policy_identity, other.resource_policy_identity);
  }
};

struct CatalogPinnedDescriptorSnapshot {
  CatalogPinnedDescriptorCacheKey key;
  EngineDescriptor descriptor;
  std::vector<EngineDescriptor> descriptors;
  EngineObjectReference descriptor_owner;
  EngineObjectReference primary_object;
  EngineResultShape result_shape;
  std::vector<std::string> evidence;
  bool read_only_snapshot = true;
  bool security_recheck_required = true;
  bool visibility_recheck_required = true;
  bool finality_authority_cached = false;
};

struct CatalogPinnedDescriptorLookupResult {
  bool ok = false;
  bool cache_hit = false;
  std::string diagnostic_code;
  std::string detail;
  std::string cache_key;
  std::shared_ptr<const CatalogPinnedDescriptorSnapshot> snapshot;
};

struct CatalogPinnedDescriptorInvalidationEvent {
  std::string event_kind;
  EngineUuid dependency_uuid;
  EngineUuid index_uuid;
  std::string security_policy_identity;
  std::string redaction_policy_identity;
  std::uint64_t event_epoch = 0;
  std::string reason;
};

struct CatalogPinnedDescriptorInvalidatedEntry {
  std::string cache_key;
  std::string descriptor_family;
  std::string reason;
  std::vector<EngineUuid> object_uuids;
  std::vector<EngineUuid> index_uuids;
};

struct CatalogPinnedDescriptorInvalidationResult {
  std::vector<CatalogPinnedDescriptorInvalidatedEntry> invalidated_entries;
};

struct CatalogPinnedDescriptorCacheStats {
  std::uint64_t puts = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t refusals = 0;
  std::uint64_t invalidations = 0;
};

class CatalogPinnedDescriptorCache {
 public:
  CatalogPinnedDescriptorLookupResult Put(CatalogPinnedDescriptorSnapshot snapshot);
  CatalogPinnedDescriptorLookupResult Lookup(const CatalogPinnedDescriptorCacheKey& key);
  CatalogPinnedDescriptorInvalidationResult Invalidate(
      const CatalogPinnedDescriptorInvalidationEvent& event);
  void Clear();
  CatalogPinnedDescriptorCacheStats Stats() const;

 private:
  mutable std::mutex mutex_;
  std::map<CatalogPinnedDescriptorCacheKey,
           std::shared_ptr<const CatalogPinnedDescriptorSnapshot>> snapshots_;
  CatalogPinnedDescriptorCacheStats stats_;
};

std::string CatalogPinnedDescriptorCacheKeyText(const CatalogPinnedDescriptorCacheKey& key);
std::string CatalogPinnedDescriptorSetDigest(const std::vector<EngineDescriptor>& descriptors,
                                             const std::vector<EngineColumnDefinition>& columns);
CatalogPinnedDescriptorLookupResult ValidateCatalogPinnedDescriptorKey(
    const CatalogPinnedDescriptorCacheKey& key);
bool CatalogPinnedDescriptorInvalidationEventKindRecognized(const std::string& event_kind);
CatalogPinnedDescriptorInvalidationEvent CatalogPinnedDescriptorInvalidationEventForMutation(
    std::string mutation_source,
    EngineUuid dependency_uuid,
    std::uint64_t event_epoch);
CatalogPinnedDescriptorCache& GlobalCatalogPinnedDescriptorCache();

}  // namespace scratchbird::engine::internal_api
