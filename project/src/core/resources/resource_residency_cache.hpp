// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IPAR-P6-18 resource residency with exact epoch invalidation.
#include "resource_seed_pack.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::core::resources {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

struct ResourceResidencyEpochVector {
  u64 catalog_epoch = 0;
  u64 resource_epoch = 0;
  u64 policy_epoch = 0;
  u64 language_epoch = 0;
  u64 charset_epoch = 0;
  u64 collation_epoch = 0;
  u64 timezone_epoch = 0;
  u64 domain_epoch = 0;
};

struct ResourceResidencyAuthority {
  bool cache_only = true;
  bool engine_mga_authoritative = true;
  bool security_recheck_required = true;
  bool parser_execution_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
};

struct ResourceResidencyEntry {
  ResourceSeedFamily family = ResourceSeedFamily::unknown;
  std::string resource_name;
  std::string version;
  std::string content_hash;
  ResourceResidencyEpochVector epoch;
  u64 bytes = 0;
  u64 last_access_tick = 0;
  u64 hit_count = 0;
  bool policy_pinned = false;
};

struct ResourceResidencyLookupResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool cache_hit = false;
  bool stale = false;
  bool fail_closed = false;
  ResourceResidencyEntry entry;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct ResourceResidencyInvalidationResult {
  u64 invalidated_count = 0;
  u64 invalidated_bytes = 0;
  std::vector<std::string> invalidated_keys;
};

struct ResourceResidencyCacheStats {
  u64 puts = 0;
  u64 hits = 0;
  u64 misses = 0;
  u64 stale_rejections = 0;
  u64 invalidations = 0;
  u64 refusals = 0;
  u64 current_bytes = 0;
  u64 max_bytes = 0;
};

class ResourceResidencyCache {
 public:
  explicit ResourceResidencyCache(u64 max_bytes);

  ResourceResidencyLookupResult Put(ResourceResidencyEntry entry,
                                    ResourceResidencyAuthority authority = {});
  ResourceResidencyLookupResult Lookup(ResourceSeedFamily family,
                                       const std::string& resource_name,
                                       const std::string& version,
                                       const ResourceResidencyEpochVector& epoch,
                                       ResourceResidencyAuthority authority = {});
  ResourceResidencyInvalidationResult InvalidateForEpoch(
      const ResourceResidencyEpochVector& current_epoch);
  void Clear();
  ResourceResidencyCacheStats Stats() const;

 private:
  ResourceResidencyLookupResult Refuse(std::string code,
                                       std::string message,
                                       std::string reason) const;
  void EvictToBudgetLocked();

  mutable std::mutex mutex_;
  std::map<std::string, ResourceResidencyEntry> entries_;
  ResourceResidencyCacheStats stats_;
};

bool ResourceResidencyEpochVectorComplete(
    const ResourceResidencyEpochVector& epoch);
bool ResourceResidencyEpochVectorMatches(
    const ResourceResidencyEpochVector& left,
    const ResourceResidencyEpochVector& right);
std::string ResourceResidencyCacheKey(ResourceSeedFamily family,
                                      const std::string& resource_name,
                                      const std::string& version);

}  // namespace scratchbird::core::resources
