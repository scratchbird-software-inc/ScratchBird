// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "catalog/catalog_object_lifecycle.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_CATALOG_DDL_SUPPORT_SERVICE_IPAR_P2
// Catalog/DDL support services operate on UUID-resolved engine requests and
// catalog state. SQL parsing, SQL text execution, and transaction finality are
// not authority here.

struct CatalogDdlDependencyEdge {
  std::string source_uuid;
  std::string source_kind;
  std::string dependency_uuid;
  std::string dependency_kind;
  std::string invalidation_action;
};

struct CatalogDdlDependencyClosure {
  std::string root_uuid;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::string cache_key;
  std::string closure_digest;
  bool cache_hit = false;
  std::vector<std::string> affected_object_uuids;
  std::vector<CatalogDdlDependencyEdge> edges;
};

struct CatalogDdlDependencyClosureCacheStats {
  std::uint64_t builds = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t clears = 0;
};

class CatalogDdlDependencyClosureCache {
 public:
  CatalogDdlDependencyClosure LookupOrBuild(
      const EngineCatalogObjectLifecycleState& state,
      const EngineRequestContext& context,
      const std::string& root_uuid);
  void Clear();
  CatalogDdlDependencyClosureCacheStats Stats() const;

 private:
  mutable std::mutex mutex_;
  std::vector<CatalogDdlDependencyClosure> closures_;
  CatalogDdlDependencyClosureCacheStats stats_;
};

CatalogDdlDependencyClosureCache& GlobalCatalogDdlDependencyClosureCache();

struct CatalogDdlStagedDescriptor {
  EngineObjectReference object;
  EngineDescriptor descriptor;
  std::string construction_phase;
  std::string validation_state;
  bool built_before_final_publish_lock = false;
  bool final_publish_lock_held = false;
  bool parser_sql_authority = false;
};

struct CatalogDdlCacheInvalidation {
  std::string cache_family;
  std::string object_uuid;
  std::string reason;
};

struct CatalogDdlPreparedContextProof {
  std::string prepared_context_uuid;
  std::vector<std::string> dependent_object_uuids;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
};

struct CatalogDdlPreparedContextInvalidation {
  std::string prepared_context_uuid;
  std::string reason;
  std::vector<std::string> matched_object_uuids;
};

struct CatalogDdlPublishPlan {
  std::uint64_t target_catalog_epoch = 0;
  std::vector<std::string> publish_steps;
  std::string final_publish_lock_scope;
  std::string rollback_recovery_authority;
  bool validation_before_publish = false;
  bool prebuild_before_final_lock = false;
  bool final_publish_short_section = false;
  bool partial_state_visible = true;
  bool uuid_returning_result = false;
};

struct CatalogDdlImmutableSnapshot {
  std::uint64_t generation = 0;
  std::uint64_t catalog_epoch = 0;
  std::string snapshot_digest;
  std::vector<std::string> object_uuids;
  bool immutable = true;
  bool finality_authority_cached = false;
};

struct CatalogDdlSnapshotReadHandle {
  std::uint64_t generation = 0;
  std::shared_ptr<const CatalogDdlImmutableSnapshot> snapshot;
};

class CatalogDdlImmutableSnapshotPublisher {
 public:
  CatalogDdlImmutableSnapshot Publish(CatalogDdlImmutableSnapshot snapshot);
  CatalogDdlSnapshotReadHandle AcquireLatest();
  void Release(const CatalogDdlSnapshotReadHandle& handle);
  std::uint64_t RetireUpTo(std::uint64_t generation);
  std::uint64_t ActiveReaders(std::uint64_t generation) const;
  std::uint64_t RetainedSnapshotCount() const;
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::uint64_t next_generation_ = 1;
  std::vector<std::shared_ptr<const CatalogDdlImmutableSnapshot>> snapshots_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> active_readers_;
};

CatalogDdlImmutableSnapshotPublisher& GlobalCatalogDdlImmutableSnapshotPublisher();

struct EngineCatalogDdlSupportRequest : EngineApiRequest {
  std::string mutation_source = "catalog_object_alter";
  bool apply_descriptor_cache_invalidation = false;
  bool publish_immutable_snapshot = true;
  std::vector<EngineObjectReference> stage_objects;
  std::vector<CatalogDdlPreparedContextProof> prepared_contexts;
};

struct EngineCatalogDdlSupportResult : EngineApiResult {
  CatalogDdlDependencyClosure dependency_closure;
  CatalogDdlPublishPlan publish_plan;
  std::vector<CatalogDdlStagedDescriptor> staged_descriptors;
  std::vector<CatalogDdlCacheInvalidation> cache_invalidations;
  std::vector<CatalogDdlPreparedContextInvalidation> prepared_context_invalidations;
  std::uint64_t immutable_snapshot_generation = 0;
  std::uint64_t descriptor_cache_invalidated_entries = 0;
};

EngineCatalogDdlSupportResult EngineCatalogDdlSupportService(
    const EngineCatalogDdlSupportRequest& request);

}  // namespace scratchbird::engine::internal_api
