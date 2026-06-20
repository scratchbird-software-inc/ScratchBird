// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::core::catalog {

using scratchbird::core::platform::u64;

struct IparCatalogAuthorityBoundary {
  bool durable_transaction_inventory_authority = true;
  bool support_service_finality_authority = false;
  bool parser_finality_authority = false;
  bool client_finality_authority = false;
  bool provider_finality_authority = false;
};

bool IparCatalogAuthorityBoundarySafe(
    const IparCatalogAuthorityBoundary& authority);

struct IparOnlineDdlRequest {
  std::string job_uuid;
  std::string object_uuid;
  std::vector<std::string> dependency_uuids;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 estimated_rows = 0;
  bool online_requested = true;
  bool dependency_conflict = false;
  bool cancellation_requested = false;
  bool recovery_replay = false;
  bool committed_metadata_snapshot_present = true;
  IparCatalogAuthorityBoundary authority;
};

struct IparOnlineDdlPlan {
  bool accepted = false;
  bool online = false;
  bool rollback_required = false;
  bool recovery_classified = false;
  bool final_publish_uses_short_exclusive_lock = false;
  std::string diagnostic_code;
  std::vector<std::string> lock_partitions;
  std::vector<std::string> progress_states;
  std::vector<std::string> evidence;
};

IparOnlineDdlPlan PlanIparOnlineDdl(const IparOnlineDdlRequest& request);

struct IparSystemMetadataRequest {
  std::string session_uuid;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  u64 language_epoch = 0;
  std::vector<std::string> requested_sys_views;
  std::vector<std::string> authorized_object_uuids;
};

struct IparSystemMetadataMaterialization {
  bool accepted = false;
  bool cache_hit = false;
  std::string diagnostic_code;
  std::string cache_key;
  u64 materialized_object_count = 0;
  std::vector<std::string> materialized_views;
  std::vector<std::string> evidence;
};

class IparSystemMetadataCache {
 public:
  IparSystemMetadataMaterialization GetOrMaterialize(
      const IparSystemMetadataRequest& request);
  void InvalidateCatalogEpoch(u64 catalog_epoch);
  std::size_t size() const { return entries_.size(); }

 private:
  std::map<std::string, IparSystemMetadataMaterialization> entries_;
};

struct IparConstraintBackfillRequest {
  std::string job_uuid;
  std::string table_uuid;
  std::string constraint_uuid;
  std::string supporting_index_uuid;
  u64 source_row_count = 0;
  u64 validated_row_count = 0;
  u64 catalog_epoch = 0;
  bool cancellation_requested = false;
  bool violation_detected = false;
  bool committed_snapshot_present = true;
  bool final_publish_requested = true;
  IparCatalogAuthorityBoundary authority;
};

struct IparConstraintBackfillPlan {
  bool accepted = false;
  bool ready_to_publish = false;
  bool rollback_required = false;
  bool partial_visible_state = false;
  std::string diagnostic_code;
  u64 remaining_rows = 0;
  std::vector<std::string> progress_states;
  std::vector<std::string> evidence;
};

IparConstraintBackfillPlan PlanIparConstraintBackfill(
    const IparConstraintBackfillRequest& request);

struct IparRuntimeRoutineCacheKey {
  std::string object_uuid;
  std::string sblr_digest;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 dependency_epoch = 0;

  bool operator<(const IparRuntimeRoutineCacheKey& other) const;
};

struct IparRuntimeRoutinePlan {
  IparRuntimeRoutineCacheKey key;
  std::string compiled_handle;
  bool trigger = false;
  bool constraint = false;
  bool routine = false;
  bool engine_sblr_only = true;
  bool parser_execution_authority = false;
};

struct IparRuntimeRoutineLookup {
  bool hit = false;
  bool accepted = false;
  std::string diagnostic_code;
  IparRuntimeRoutinePlan plan;
  std::vector<std::string> evidence;
};

class IparRuntimeRoutineCache {
 public:
  IparRuntimeRoutineLookup Put(IparRuntimeRoutinePlan plan);
  IparRuntimeRoutineLookup Lookup(const IparRuntimeRoutineCacheKey& key) const;
  u64 InvalidateDependencyEpoch(u64 dependency_epoch);

 private:
  std::map<IparRuntimeRoutineCacheKey, IparRuntimeRoutinePlan> entries_;
};

}  // namespace scratchbird::core::catalog
