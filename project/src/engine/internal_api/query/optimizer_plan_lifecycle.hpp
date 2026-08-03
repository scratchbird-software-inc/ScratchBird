// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-OPTIMIZER-PLAN-LIFECYCLE-ANCHOR
#include "descriptor_value_runtime.hpp"
#include "index_statistics_lifecycle.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

namespace index_lifecycle = scratchbird::core::index;
namespace plan_executor = scratchbird::engine::executor;

inline constexpr const char* kOptimizerPlanLifecycleEventMagic = "SBPLANL2";
inline constexpr const char* kOptimizerPlanLifecycleLegacyEventMagic =
    "SBPLANL1";
inline constexpr std::uint32_t kOptimizerPlanLifecycleEventSchemaVersion = 2;

inline constexpr const char* kOptimizerPlanDiagnosticOk = "OPTIMIZER.PLAN.OK";
inline constexpr const char* kOptimizerPlanDiagnosticDatabasePathRequired =
    "OPTIMIZER.PLAN.DATABASE_PATH_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticMgaTransactionRequired =
    "OPTIMIZER.PLAN.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticMgaAuthorityRequired =
    "OPTIMIZER.PLAN.MGA_AUTHORITY_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticCatalogIdentityRequired =
    "OPTIMIZER.PLAN.CATALOG_IDENTITY_REQUIRED";
inline constexpr const char* kOptimizerPlanDiagnosticDependencyMismatch =
    "OPTIMIZER.PLAN.DEPENDENCY_MISMATCH";
inline constexpr const char* kOptimizerPlanDiagnosticInvalidRequest =
    "OPTIMIZER.PLAN.INVALID_REQUEST";
inline constexpr const char* kOptimizerPlanDiagnosticStatisticsStale =
    "OPTIMIZER.PLAN.STATISTICS_STALE";
inline constexpr const char* kOptimizerPlanDiagnosticCacheMiss =
    "OPTIMIZER.PLAN.CACHE_MISS";
inline constexpr const char* kOptimizerPlanDiagnosticCacheInvalidated =
    "OPTIMIZER.PLAN.CACHE_INVALIDATED";
inline constexpr const char* kOptimizerPlanDiagnosticEpochMismatch =
    "OPTIMIZER.PLAN.EPOCH_MISMATCH";
inline constexpr const char* kOptimizerPlanDiagnosticWriteFailed =
    "OPTIMIZER.PLAN.WRITE_FAILED";

// Shared lifecycle entries retain only immutable optimizer metadata. The
// exact statement snapshot, current resolver, visibility and finality remain
// outside this structure and outside the durable event stream.
struct EngineOptimizerPlanDependencyIdentity {
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t statistics_generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t route_generation = 0;
  std::vector<std::string> object_dependency_uuids;
};

struct EngineOptimizerPlanCacheEntry {
  std::uint32_t event_schema_version =
      kOptimizerPlanLifecycleEventSchemaVersion;
  std::string event_uuid;
  std::uint64_t event_sequence = 0;
  std::uint64_t plan_cache_epoch = 0;
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string relation_uuid;
  std::string index_uuid;
  std::string catalog_physical_profile_key;
  std::string plan_shape_digest;
  std::uint64_t index_generation = 0;
  std::uint64_t statistics_generation = 0;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t charset_epoch = 0;
  std::uint64_t collation_epoch = 0;
  EngineOptimizerPlanDependencyIdentity dependencies;
  bool metadata_only = true;
  bool invalidated = false;
  std::string invalidation_reason;
  bool recovered_from_persisted_evidence = false;
};

struct EngineOptimizerPlanLifecycleState {
  std::vector<EngineOptimizerPlanCacheEntry> entries;
  std::uint64_t plan_cache_epoch = 0;
  std::uint64_t max_event_sequence = 0;
  std::uint64_t invalidation_events = 0;
  std::uint64_t rejected_event_count = 0;
  std::uint64_t legacy_event_count = 0;
  std::uint64_t malformed_event_count = 0;
  bool recovered_from_persisted_evidence = false;
  std::string recovery_snapshot_uuid;
};

struct EngineLoadOptimizerPlanLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineOptimizerPlanLifecycleState state;
};

struct EngineOptimizerCachePlanRequest : EngineApiRequest {
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string relation_uuid;
  std::string index_uuid;
  std::string plan_shape_digest;
  plan_executor::CanonicalExecutionMgaAuthority mga_authority;
  plan_executor::TypedPhysicalNodeDag selected_physical_dag;
  std::string selected_catalog_epoch_uuid;
  std::vector<std::string> object_dependency_uuids;
  index_lifecycle::IndexLifecycleDescriptor index_descriptor;
  index_lifecycle::IndexStatisticsSnapshot statistics;
  index_lifecycle::IndexStatisticsFreshnessPolicy freshness_policy =
      index_lifecycle::IndexStatisticsFreshnessPolicy::refuse_stale;
};

struct EngineOptimizerCachePlanResult : EngineApiResult {
  EngineOptimizerPlanCacheEntry entry;
  std::uint64_t plan_cache_epoch = 0;
  std::shared_ptr<const struct EngineOptimizerPlanStatementUseReceipt>
      publication_receipt;
};
EngineOptimizerCachePlanResult EngineOptimizerCachePlan(
    const EngineOptimizerCachePlanRequest& request);

struct EngineOptimizerValidateCachedPlanRequest : EngineApiRequest {
  std::string plan_uuid;
  std::string query_fingerprint;
  std::string index_uuid;
  plan_executor::CanonicalExecutionMgaAuthority mga_authority;
  plan_executor::TypedPhysicalNodeDag selected_physical_dag;
  std::string selected_catalog_epoch_uuid;
  std::vector<std::string> object_dependency_uuids;
  std::uint64_t current_index_generation = 0;
  std::uint64_t current_statistics_generation = 0;
  std::uint64_t current_catalog_generation_id = 0;
  index_lifecycle::IndexResourceEpochVector current_resource_epochs;
  bool require_current_statistics = true;
  bool statistics_stale = false;
};

enum class EngineOptimizerPlanReceiptPurpose : std::uint8_t {
  kPublication = 1,
  kStatementUse,
};

struct EngineOptimizerPlanStatementUseReceipt;
struct EngineOptimizerPlanUseValidationResult;

struct EngineOptimizerValidateCachedPlanResult : EngineApiResult {
  EngineOptimizerPlanCacheEntry entry;
  bool cache_hit = false;
  bool metadata_cache_hit = false;
  bool statement_use_admitted = false;
  bool invalidation_required = false;
  std::uint64_t plan_cache_epoch = 0;
  std::shared_ptr<const EngineOptimizerPlanStatementUseReceipt>
      statement_use_receipt;
};

// A receipt is privately constructed after the metadata hit, every dependency
// comparison, and a second exact current-engine MGA revalidation. It is not a
// cache entry or a durable event and cannot be assembled from scalar fields.
struct EngineOptimizerPlanStatementUseReceipt {
 public:
  const std::string& receipt_id() const noexcept { return receipt_id_; }
  const std::string& plan_uuid() const noexcept { return plan_uuid_; }
  const std::string& catalog_epoch_uuid() const noexcept {
    return dependencies_.catalog_epoch_uuid;
  }
  const plan_executor::PhysicalMgaStatementContext& statement_context()
      const noexcept {
    return statement_context_;
  }
  plan_executor::CanonicalMgaAuthorityOrigin authority_origin() const noexcept {
    return authority_origin_;
  }
  EngineOptimizerPlanReceiptPurpose purpose() const noexcept {
    return purpose_;
  }

 private:
  friend EngineOptimizerCachePlanResult EngineOptimizerCachePlan(
      const EngineOptimizerCachePlanRequest& request);
  friend EngineOptimizerValidateCachedPlanResult
  EngineOptimizerValidateCachedPlan(
      const EngineOptimizerValidateCachedPlanRequest& request);
  friend struct EngineOptimizerPlanUseValidationResult;
  friend EngineOptimizerPlanUseValidationResult
  RevalidateOptimizerPlanStatementUse(
      const EngineOptimizerPlanCacheEntry& entry,
      const std::shared_ptr<const EngineOptimizerPlanStatementUseReceipt>&
          receipt);

  EngineOptimizerPlanStatementUseReceipt() = default;

  std::string receipt_id_;
  std::string plan_uuid_;
  EngineOptimizerPlanDependencyIdentity dependencies_;
  plan_executor::PhysicalMgaStatementContext statement_context_;
  plan_executor::TypedPhysicalNodeDag selected_physical_dag_;
  plan_executor::CanonicalMgaCurrentResolver resolve_current_;
  plan_executor::CanonicalMgaAuthorityOrigin authority_origin_ =
      plan_executor::CanonicalMgaAuthorityOrigin::kMissing;
  EngineOptimizerPlanReceiptPurpose purpose_ =
      EngineOptimizerPlanReceiptPurpose::kStatementUse;
  bool metadata_dependencies_revalidated_ = false;
  bool exact_current_revalidated_before_issue_ = false;
};

EngineOptimizerValidateCachedPlanResult EngineOptimizerValidateCachedPlan(
    const EngineOptimizerValidateCachedPlanRequest& request);

struct EngineOptimizerPlanUseValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<std::string> evidence;
  std::shared_ptr<const EngineOptimizerPlanStatementUseReceipt>
      executable_receipt;
};

EngineOptimizerPlanUseValidationResult RevalidateOptimizerPlanStatementUse(
    const EngineOptimizerPlanCacheEntry& entry,
    const std::shared_ptr<const EngineOptimizerPlanStatementUseReceipt>&
        receipt);

struct EngineOptimizerInvalidatePlanCacheRequest : EngineApiRequest {
  std::string index_uuid;
  std::string reason = "explicit_invalidation";
  std::uint64_t new_index_generation = 0;
  std::uint64_t new_statistics_generation = 0;
  std::uint64_t new_catalog_generation_id = 0;
  index_lifecycle::IndexResourceEpochVector new_resource_epochs;
  bool invalidate_all = false;
};

struct EngineOptimizerInvalidatePlanCacheResult : EngineApiResult {
  EngineOptimizerPlanLifecycleState state;
  std::uint64_t plan_cache_epoch = 0;
};
EngineOptimizerInvalidatePlanCacheResult EngineOptimizerInvalidatePlanCache(
    const EngineOptimizerInvalidatePlanCacheRequest& request);

struct EngineOptimizerRecoverPlanCacheRequest : EngineApiRequest {};

struct EngineOptimizerRecoverPlanCacheResult : EngineApiResult {
  EngineOptimizerPlanLifecycleState state;
  std::string recovery_snapshot_uuid;
};
EngineOptimizerRecoverPlanCacheResult EngineOptimizerRecoverPlanCache(
    const EngineOptimizerRecoverPlanCacheRequest& request);

EngineLoadOptimizerPlanLifecycleStateResult LoadOptimizerPlanLifecycleState(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
