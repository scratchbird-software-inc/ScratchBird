// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/pinned_descriptor_cache.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

void AppendSorted(std::ostringstream& out, const char* label, std::vector<std::string> values) {
  out << '|' << label << '=';
  for (const auto& value : SortedUnique(std::move(values))) out << value << ';';
}

std::string UInt64Hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableDigest(const std::vector<std::string>& parts) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& part : parts) {
    for (const unsigned char ch : part) {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return "fnv1a64:" + UInt64Hex(hash);
}

std::string DescriptorText(const EngineDescriptor& descriptor) {
  return descriptor.descriptor_uuid.canonical + ":" + descriptor.descriptor_kind + ":" +
         descriptor.canonical_type_name + ":" + descriptor.encoded_descriptor;
}

CatalogPinnedDescriptorLookupResult Refusal(std::string code,
                                            std::string detail,
                                            std::string cache_key = {}) {
  CatalogPinnedDescriptorLookupResult result;
  result.ok = false;
  result.cache_hit = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.cache_key = std::move(cache_key);
  return result;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return !value.empty() &&
         std::find(values.begin(), values.end(), value) != values.end();
}

bool InvalidatesAll(const CatalogPinnedDescriptorInvalidationEvent& event) {
  return event.event_kind == "catalog_epoch" ||
         event.event_kind == "security_epoch" ||
         event.event_kind == "resource_epoch" ||
         event.event_kind == "policy_epoch" ||
         event.event_kind == "name_resolution_epoch" ||
         event.event_kind == "stats_epoch" ||
         event.event_kind == "stats_refresh" ||
         event.event_kind == "statistics_refresh" ||
         event.event_kind == "redaction_epoch" ||
         event.event_kind == "redaction_policy_epoch" ||
         event.event_kind == "nosql_generation_publish" ||
         event.event_kind == "nosql_generation_retire" ||
         event.event_kind == "nosql_compaction" ||
         event.event_kind == "nosql_family_compaction";
}

bool InvalidatesByDependency(const CatalogPinnedDescriptorInvalidationEvent& event) {
  return event.event_kind == "catalog_create" ||
         event.event_kind == "catalog_alter" ||
         event.event_kind == "catalog_drop" ||
         event.event_kind == "ddl_catalog_mutation" ||
         event.event_kind == "index_change" ||
         event.event_kind == "security_policy_change" ||
         event.event_kind == "redaction_policy_change" ||
         event.event_kind == "resource_policy_change" ||
         event.event_kind == "datatype_descriptor_change" ||
         event.event_kind == "domain_change" ||
         event.event_kind == "collation_change" ||
         event.event_kind == "udr_change";
}

bool EventInvalidatesSnapshot(const CatalogPinnedDescriptorSnapshot& snapshot,
                              const CatalogPinnedDescriptorInvalidationEvent& event) {
  if (!CatalogPinnedDescriptorInvalidationEventKindRecognized(event.event_kind)) return true;
  if (InvalidatesAll(event)) return true;
  if (!event.index_uuid.empty() && Contains(snapshot.key.index_uuids, event.index_uuid)) return true;
  if (!event.dependency_uuid.empty() && Contains(snapshot.key.object_uuids, event.dependency_uuid)) return true;
  if (!event.security_policy_identity.empty() &&
      event.security_policy_identity == snapshot.key.security_policy_identity) {
    return true;
  }
  if (!event.redaction_policy_identity.empty() &&
      event.redaction_policy_identity == snapshot.key.redaction_policy_identity) {
    return true;
  }
  return InvalidatesByDependency(event) &&
         event.dependency_uuid.empty() &&
         event.index_uuid.empty() &&
         event.security_policy_identity.empty() &&
         event.redaction_policy_identity.empty();
}

}  // namespace

std::string CatalogPinnedDescriptorCacheKeyText(const CatalogPinnedDescriptorCacheKey& key) {
  std::ostringstream out;
  out << "family=" << key.descriptor_family
      << "|catalog_epoch=" << key.catalog_epoch
      << "|security_epoch=" << key.security_epoch
      << "|resource_policy_epoch=" << key.resource_policy_epoch
      << "|name_resolution_epoch=" << key.name_resolution_epoch
      << "|stats_epoch_relevant=" << (key.stats_epoch_relevant ? "true" : "false")
      << "|stats_epoch=" << key.stats_epoch
      << "|descriptor_set_digest=" << key.descriptor_set_digest
      << "|security_policy_identity=" << key.security_policy_identity
      << "|redaction_policy_identity=" << key.redaction_policy_identity
      << "|resource_policy_identity=" << key.resource_policy_identity;
  AppendSorted(out, "object_uuids", key.object_uuids);
  AppendSorted(out, "index_uuids", key.index_uuids);
  return out.str();
}

std::string CatalogPinnedDescriptorSetDigest(const std::vector<EngineDescriptor>& descriptors,
                                             const std::vector<EngineColumnDefinition>& columns) {
  std::vector<std::string> parts;
  parts.reserve(descriptors.size() + columns.size());
  for (const auto& descriptor : descriptors) {
    parts.push_back("descriptor:" + DescriptorText(descriptor));
  }
  for (const auto& column : columns) {
    parts.push_back("column:" + column.requested_column_uuid.canonical + ":" +
                    std::to_string(column.ordinal) + ":" +
                    (column.nullable ? "nullable" : "required") + ":" +
                    DescriptorText(column.descriptor));
  }
  return StableDigest(parts);
}

CatalogPinnedDescriptorLookupResult ValidateCatalogPinnedDescriptorKey(
    const CatalogPinnedDescriptorCacheKey& key) {
  const std::string cache_key = CatalogPinnedDescriptorCacheKeyText(key);
  if (key.descriptor_family.empty()) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_FAMILY_REQUIRED",
                   "descriptor family is required",
                   cache_key);
  }
  if (key.catalog_epoch == 0) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
                   "catalog_epoch is required",
                   cache_key);
  }
  if (key.security_epoch == 0) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
                   "security_epoch is required",
                   cache_key);
  }
  if (key.resource_policy_epoch == 0) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
                   "resource_policy_epoch is required",
                   cache_key);
  }
  if (key.name_resolution_epoch == 0) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
                   "name_resolution_epoch is required",
                   cache_key);
  }
  if (key.stats_epoch_relevant && key.stats_epoch == 0) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
                   "stats_epoch is required for statistics descriptors",
                   cache_key);
  }
  if (key.descriptor_set_digest.empty()) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_DIGEST_REQUIRED",
                   "descriptor_set_digest is required",
                   cache_key);
  }
  if (key.object_uuids.empty()) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_OBJECT_UUID_REQUIRED",
                   "at least one object UUID is required",
                   cache_key);
  }
  if (key.security_policy_identity.empty()) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_SECURITY_POLICY_REQUIRED",
                   "security policy identity is required",
                   cache_key);
  }
  if (key.redaction_policy_identity.empty()) {
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_REDACTION_POLICY_REQUIRED",
                   "redaction policy identity is required",
                   cache_key);
  }

  CatalogPinnedDescriptorLookupResult result;
  result.ok = true;
  result.cache_key = cache_key;
  result.diagnostic_code = "SB_CATALOG_PINNED_DESCRIPTOR_KEY_OK";
  return result;
}

CatalogPinnedDescriptorLookupResult CatalogPinnedDescriptorCache::Put(
    CatalogPinnedDescriptorSnapshot snapshot) {
  auto validation = ValidateCatalogPinnedDescriptorKey(snapshot.key);
  if (!validation.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return validation;
  }
  if (!snapshot.read_only_snapshot ||
      !snapshot.security_recheck_required ||
      !snapshot.visibility_recheck_required ||
      snapshot.finality_authority_cached) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_UNSAFE_SNAPSHOT",
                   "pinned descriptor snapshots must be read-only metadata and preserve security/MGA rechecks",
                   validation.cache_key);
  }

  auto stored = std::make_shared<const CatalogPinnedDescriptorSnapshot>(std::move(snapshot));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_[validation.cache_key] = stored;
    ++stats_.puts;
  }

  validation.snapshot = std::move(stored);
  validation.cache_hit = false;
  validation.diagnostic_code = "SB_CATALOG_PINNED_DESCRIPTOR_PUT";
  return validation;
}

CatalogPinnedDescriptorLookupResult CatalogPinnedDescriptorCache::Lookup(
    const CatalogPinnedDescriptorCacheKey& key) {
  auto validation = ValidateCatalogPinnedDescriptorKey(key);
  if (!validation.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return validation;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto found = snapshots_.find(validation.cache_key);
  if (found == snapshots_.end()) {
    ++stats_.misses;
    return Refusal("SB_CATALOG_PINNED_DESCRIPTOR_CACHE_MISS",
                   "epoch-pinned descriptor snapshot not found",
                   validation.cache_key);
  }
  ++stats_.hits;
  validation.cache_hit = true;
  validation.diagnostic_code = "SB_CATALOG_PINNED_DESCRIPTOR_CACHE_HIT";
  validation.snapshot = found->second;
  return validation;
}

CatalogPinnedDescriptorInvalidationResult CatalogPinnedDescriptorCache::Invalidate(
    const CatalogPinnedDescriptorInvalidationEvent& event) {
  CatalogPinnedDescriptorInvalidationResult result;
  const std::string reason = event.reason.empty() ? event.event_kind : event.reason;

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = snapshots_.begin(); it != snapshots_.end();) {
    if (!EventInvalidatesSnapshot(*it->second, event)) {
      ++it;
      continue;
    }
    CatalogPinnedDescriptorInvalidatedEntry entry;
    entry.cache_key = it->first;
    entry.descriptor_family = it->second->key.descriptor_family;
    entry.reason = reason;
    entry.object_uuids = it->second->key.object_uuids;
    entry.index_uuids = it->second->key.index_uuids;
    result.invalidated_entries.push_back(std::move(entry));
    it = snapshots_.erase(it);
  }
  stats_.invalidations += result.invalidated_entries.size();
  return result;
}

void CatalogPinnedDescriptorCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.invalidations += snapshots_.size();
  snapshots_.clear();
}

CatalogPinnedDescriptorCacheStats CatalogPinnedDescriptorCache::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

bool CatalogPinnedDescriptorInvalidationEventKindRecognized(const std::string& event_kind) {
  return event_kind == "catalog_epoch" ||
         event_kind == "security_epoch" ||
         event_kind == "resource_epoch" ||
         event_kind == "policy_epoch" ||
         event_kind == "name_resolution_epoch" ||
         event_kind == "stats_epoch" ||
         event_kind == "stats_refresh" ||
         event_kind == "statistics_refresh" ||
         event_kind == "redaction_epoch" ||
         event_kind == "redaction_policy_epoch" ||
         event_kind == "nosql_generation_publish" ||
         event_kind == "nosql_generation_retire" ||
         event_kind == "nosql_compaction" ||
         event_kind == "nosql_family_compaction" ||
         event_kind == "catalog_create" ||
         event_kind == "catalog_alter" ||
         event_kind == "catalog_drop" ||
         event_kind == "ddl_catalog_mutation" ||
         event_kind == "index_change" ||
         event_kind == "security_policy_change" ||
         event_kind == "redaction_policy_change" ||
         event_kind == "resource_policy_change" ||
         event_kind == "datatype_descriptor_change" ||
         event_kind == "domain_change" ||
         event_kind == "collation_change" ||
         event_kind == "udr_change";
}

CatalogPinnedDescriptorInvalidationEvent CatalogPinnedDescriptorInvalidationEventForMutation(
    std::string mutation_source,
    std::string dependency_uuid,
    std::uint64_t event_epoch) {
  CatalogPinnedDescriptorInvalidationEvent event;
  event.dependency_uuid = std::move(dependency_uuid);
  event.event_epoch = event_epoch;
  if (mutation_source == "catalog_object_create") {
    event.event_kind = "catalog_create";
  } else if (mutation_source == "catalog_object_alter") {
    event.event_kind = "catalog_alter";
  } else if (mutation_source == "catalog_object_drop") {
    event.event_kind = "catalog_drop";
  } else if (mutation_source == "ddl_catalog_mutation") {
    event.event_kind = "ddl_catalog_mutation";
  } else if (mutation_source == "index_create" ||
             mutation_source == "index_alter" ||
             mutation_source == "index_drop") {
    event.event_kind = "index_change";
  } else if (mutation_source == "security_epoch_change" ||
             mutation_source == "security_policy_mutation") {
    event.event_kind = "security_policy_change";
  } else if (mutation_source == "redaction_epoch_change" ||
             mutation_source == "redaction_policy_mutation") {
    event.event_kind = "redaction_policy_change";
  } else if (mutation_source == "statistics_refresh" || mutation_source == "stats_refresh") {
    event.event_kind = "statistics_refresh";
  } else if (mutation_source == "nosql_generation_publication" ||
             mutation_source == "nosql_generation_publish") {
    event.event_kind = "nosql_generation_publish";
  } else if (mutation_source == "nosql_generation_retirement" ||
             mutation_source == "nosql_generation_retire") {
    event.event_kind = "nosql_generation_retire";
  } else if (mutation_source == "nosql_compaction" ||
             mutation_source == "nosql_family_compaction") {
    event.event_kind = "nosql_compaction";
  } else {
    event.event_kind = std::move(mutation_source);
  }
  event.reason = event.event_kind;
  return event;
}

CatalogPinnedDescriptorCache& GlobalCatalogPinnedDescriptorCache() {
  static CatalogPinnedDescriptorCache cache;
  return cache;
}

}  // namespace scratchbird::engine::internal_api
