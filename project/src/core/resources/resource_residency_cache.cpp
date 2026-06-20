// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resource_residency_cache.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::resources {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAnchor =
    "IPAR-P6-18_DOMAIN_CHARSET_COLLATION_TIMEZONE_LANGUAGE_RESIDENCY";
constexpr const char* kAuthorityScope =
    "resource_residency.cache_only_no_transaction_visibility_security_recovery_or_parser_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

DiagnosticRecord Diagnostic(Status status,
                            std::string code,
                            std::string message,
                            std::vector<DiagnosticArgument> args = {}) {
  args.push_back({"slice", kAnchor});
  args.push_back({"authority_scope", kAuthorityScope});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(code),
                        std::move(message),
                        std::move(args),
                        {},
                        "core.resources.resource_residency_cache",
                        "reload or invalidate resource residency state at catalog, policy, language, charset, collation, timezone, or domain epoch changes");
}

bool UnsafeAuthority(const ResourceResidencyAuthority& authority,
                     std::string* reason) {
  if (!authority.cache_only) {
    *reason = "resource_residency_must_be_cache_only";
    return true;
  }
  if (!authority.engine_mga_authoritative) {
    *reason = "engine_mga_authority_required";
    return true;
  }
  if (!authority.security_recheck_required) {
    *reason = "security_recheck_required";
    return true;
  }
  if (authority.parser_execution_authority ||
      authority.transaction_finality_authority ||
      authority.recovery_authority) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

bool LessValuable(const ResourceResidencyEntry& left,
                  const ResourceResidencyEntry& right) {
  if (left.policy_pinned != right.policy_pinned) {
    return !left.policy_pinned && right.policy_pinned;
  }
  const u64 left_score = left.hit_count * 8 + left.last_access_tick;
  const u64 right_score = right.hit_count * 8 + right.last_access_tick;
  if (left_score != right_score) {
    return left_score < right_score;
  }
  return left.bytes > right.bytes;
}

}  // namespace

ResourceResidencyCache::ResourceResidencyCache(u64 max_bytes) {
  stats_.max_bytes = max_bytes;
}

bool ResourceResidencyEpochVectorComplete(
    const ResourceResidencyEpochVector& epoch) {
  return epoch.catalog_epoch != 0 &&
         epoch.resource_epoch != 0 &&
         epoch.policy_epoch != 0 &&
         epoch.language_epoch != 0 &&
         epoch.charset_epoch != 0 &&
         epoch.collation_epoch != 0 &&
         epoch.timezone_epoch != 0 &&
         epoch.domain_epoch != 0;
}

bool ResourceResidencyEpochVectorMatches(
    const ResourceResidencyEpochVector& left,
    const ResourceResidencyEpochVector& right) {
  return left.catalog_epoch == right.catalog_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.language_epoch == right.language_epoch &&
         left.charset_epoch == right.charset_epoch &&
         left.collation_epoch == right.collation_epoch &&
         left.timezone_epoch == right.timezone_epoch &&
         left.domain_epoch == right.domain_epoch;
}

std::string ResourceResidencyCacheKey(ResourceSeedFamily family,
                                      const std::string& resource_name,
                                      const std::string& version) {
  return std::string(ResourceSeedFamilyName(family)) + "|" +
         resource_name + "|" + version;
}

ResourceResidencyLookupResult ResourceResidencyCache::Refuse(
    std::string code,
    std::string message,
    std::string reason) const {
  ResourceResidencyLookupResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = Diagnostic(result.status,
                                 std::move(code),
                                 std::move(message),
                                 {{"reason", std::move(reason)}});
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("resource_residency.fail_closed=true");
  return result;
}

ResourceResidencyLookupResult ResourceResidencyCache::Put(
    ResourceResidencyEntry entry,
    ResourceResidencyAuthority authority) {
  std::string unsafe_reason;
  if (UnsafeAuthority(authority, &unsafe_reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return Refuse("SB_RESOURCE_RESIDENCY.UNSAFE_AUTHORITY",
                  "resource.residency.unsafe_authority",
                  std::move(unsafe_reason));
  }
  if (stats_.max_bytes == 0 || entry.bytes == 0 ||
      entry.bytes > stats_.max_bytes ||
      entry.family == ResourceSeedFamily::unknown ||
      entry.resource_name.empty() ||
      entry.version.empty() ||
      entry.content_hash.empty() ||
      !ResourceResidencyEpochVectorComplete(entry.epoch)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return Refuse("SB_RESOURCE_RESIDENCY.ENTRY_INVALID",
                  "resource.residency.entry_invalid",
                  "family_name_version_hash_epoch_and_size_required");
  }

  const std::string key =
      ResourceResidencyCacheKey(entry.family, entry.resource_name, entry.version);
  ResourceResidencyLookupResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
      stats_.current_bytes -= existing->second.bytes;
    }
    stats_.current_bytes += entry.bytes;
    entries_[key] = entry;
    ++stats_.puts;
    EvictToBudgetLocked();
  }

  result.status = OkStatus();
  result.entry = std::move(entry);
  result.diagnostic = Diagnostic(result.status,
                                 "SB_RESOURCE_RESIDENCY.PUT",
                                 "resource.residency.put",
                                 {{"key", key}});
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("resource_residency.policy_bounded=true");
  result.evidence.push_back("resource_residency.exact_epoch_invalidation=true");
  return result;
}

ResourceResidencyLookupResult ResourceResidencyCache::Lookup(
    ResourceSeedFamily family,
    const std::string& resource_name,
    const std::string& version,
    const ResourceResidencyEpochVector& epoch,
    ResourceResidencyAuthority authority) {
  std::string unsafe_reason;
  if (UnsafeAuthority(authority, &unsafe_reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return Refuse("SB_RESOURCE_RESIDENCY.UNSAFE_AUTHORITY",
                  "resource.residency.unsafe_authority",
                  std::move(unsafe_reason));
  }
  if (!ResourceResidencyEpochVectorComplete(epoch)) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.refusals;
    return Refuse("SB_RESOURCE_RESIDENCY.EPOCH_INVALID",
                  "resource.residency.epoch_invalid",
                  "complete_epoch_vector_required");
  }

  const std::string key =
      ResourceResidencyCacheKey(family, resource_name, version);
  ResourceResidencyLookupResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    ++stats_.misses;
    result.status = ErrorStatus();
    result.cache_hit = false;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_RESOURCE_RESIDENCY.MISS",
                                   "resource.residency.miss",
                                   {{"key", key}});
    result.evidence.push_back(kAnchor);
    result.evidence.push_back(kAuthorityScope);
    return result;
  }
  if (!ResourceResidencyEpochVectorMatches(found->second.epoch, epoch)) {
    ++stats_.stale_rejections;
    result.status = ErrorStatus();
    result.stale = true;
    result.diagnostic = Diagnostic(result.status,
                                   "SB_RESOURCE_RESIDENCY.STALE",
                                   "resource.residency.stale",
                                   {{"key", key}});
    result.evidence.push_back(kAnchor);
    result.evidence.push_back(kAuthorityScope);
    result.evidence.push_back("resource_residency.stale_refused=true");
    return result;
  }
  ++stats_.hits;
  found->second.hit_count += 1;
  result.status = OkStatus();
  result.cache_hit = true;
  result.entry = found->second;
  result.diagnostic = Diagnostic(result.status,
                                 "SB_RESOURCE_RESIDENCY.HIT",
                                 "resource.residency.hit",
                                 {{"key", key}});
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kAuthorityScope);
  return result;
}

ResourceResidencyInvalidationResult ResourceResidencyCache::InvalidateForEpoch(
    const ResourceResidencyEpochVector& current_epoch) {
  ResourceResidencyInvalidationResult result;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (ResourceResidencyEpochVectorMatches(it->second.epoch, current_epoch)) {
      ++it;
      continue;
    }
    result.invalidated_bytes += it->second.bytes;
    result.invalidated_keys.push_back(it->first);
    stats_.current_bytes -= it->second.bytes;
    it = entries_.erase(it);
  }
  result.invalidated_count = result.invalidated_keys.size();
  stats_.invalidations += result.invalidated_count;
  return result;
}

void ResourceResidencyCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.invalidations += entries_.size();
  entries_.clear();
  stats_.current_bytes = 0;
}

ResourceResidencyCacheStats ResourceResidencyCache::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void ResourceResidencyCache::EvictToBudgetLocked() {
  while (stats_.current_bytes > stats_.max_bytes && !entries_.empty()) {
    auto victim = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (LessValuable(it->second, victim->second)) {
        victim = it;
      }
    }
    if (victim->second.policy_pinned) {
      break;
    }
    stats_.current_bytes -= victim->second.bytes;
    entries_.erase(victim);
    ++stats_.invalidations;
  }
}

}  // namespace scratchbird::core::resources
