// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-STATISTICS-LIFECYCLE-ANCHOR
#include "catalog_index_profile.hpp"
#include "index_access_method.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

inline constexpr const char* kIndexStatisticsDiagnosticOk = "INDEX.STATISTICS.OK";
inline constexpr const char* kIndexStatisticsDiagnosticInvalidRequest =
    "INDEX.STATISTICS.INVALID_REQUEST";
inline constexpr const char* kIndexStatisticsDiagnosticMgaTransactionRequired =
    "INDEX.STATISTICS.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kIndexStatisticsDiagnosticCatalogProfileMissing =
    "INDEX.STATISTICS.CATALOG_PROFILE_MISSING";
inline constexpr const char* kIndexStatisticsDiagnosticCatalogProfileMismatch =
    "INDEX.STATISTICS.CATALOG_PROFILE_MISMATCH";
inline constexpr const char* kIndexStatisticsDiagnosticResourceEpochMismatch =
    "INDEX.STATISTICS.RESOURCE_EPOCH_MISMATCH";
inline constexpr const char* kIndexStatisticsDiagnosticLifecycleStateRefused =
    "INDEX.STATISTICS.LIFECYCLE_STATE_REFUSED";
inline constexpr const char* kIndexStatisticsDiagnosticStaleRefused =
    "INDEX.STATISTICS.STALE_REFUSED";
inline constexpr const char* kIndexStatisticsDiagnosticRecoveryClassificationRequired =
    "INDEX.STATISTICS.RECOVERY_CLASSIFICATION_REQUIRED";

enum class IndexStatisticsLifecycleOperation : std::uint16_t {
  build,
  drop,
  rebuild,
  refresh_statistics,
  classify_recovery
};

enum class IndexStatisticsLifecycleState : std::uint16_t {
  absent,
  build_pending,
  building,
  ready,
  dropping,
  dropped,
  stale,
  suspect,
  quarantine
};

enum class IndexStatisticsFreshnessPolicy : std::uint16_t {
  allow_stale_with_refresh,
  refuse_stale,
  require_current
};

enum class IndexRecoveryClassification : std::uint16_t {
  clean_ready,
  interrupted_build,
  interrupted_drop,
  interrupted_rebuild,
  interrupted_statistics_refresh,
  stale_resource_epoch,
  corrupt_evidence,
  quarantine_required
};

struct IndexResourceEpochVector {
  u64 resource_epoch = 0;
  u64 charset_epoch = 0;
  u64 collation_epoch = 0;
};

struct IndexCatalogProfileBinding {
  std::string physical_profile_key;
  std::string catalog_table_path;
  bool catalog_profile_authoritative = false;
  bool catalog_profile_supports_mga_snapshot_visibility = false;
  bool catalog_profile_supports_exact_lookup = false;
  bool catalog_profile_supports_generation_visibility = false;
};

struct IndexLifecycleDescriptor {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexStatisticsLifecycleState lifecycle_state = IndexStatisticsLifecycleState::absent;
  u64 catalog_generation_id = 0;
  u64 index_generation = 0;
  u64 metadata_epoch = 0;
  u64 creating_transaction_id = 0;
  u64 dropping_transaction_id = 0;
  IndexResourceEpochVector resource_epochs;
  IndexCatalogProfileBinding catalog_profile;
  bool unique = false;
  bool online_build = false;
  bool build_validation_complete = false;
  bool drop_retention_blocked = false;
};

struct IndexStatisticsSnapshot {
  TypedUuid index_uuid;
  u64 statistics_generation = 0;
  u64 index_generation = 0;
  u64 catalog_generation_id = 0;
  u64 refreshed_by_transaction_id = 0;
  u64 visible_generation = 0;
  u64 row_count = 0;
  u64 distinct_key_count = 0;
  u64 leaf_page_count = 0;
  u64 retained_version_count = 0;
  double selectivity = 1.0;
  IndexResourceEpochVector resource_epochs;
  std::string physical_profile_key;
  bool catalog_profile_coupled = false;
  bool current = false;
  bool stale = false;
  bool mga_visible = false;
};

struct IndexStatisticsRefreshInput {
  IndexStatisticsSnapshot prior_statistics;
  u64 observed_row_count = 0;
  u64 observed_distinct_key_count = 0;
  u64 observed_leaf_page_count = 0;
  u64 observed_retained_version_count = 0;
  bool full_scan_evidence = false;
  bool sampled_evidence = false;
};

struct IndexStatisticsLifecycleRequest {
  IndexStatisticsLifecycleOperation operation = IndexStatisticsLifecycleOperation::build;
  IndexLifecycleDescriptor descriptor;
  IndexStatisticsRefreshInput refresh;
  IndexStatisticsFreshnessPolicy freshness_policy = IndexStatisticsFreshnessPolicy::refuse_stale;
  u64 local_transaction_id = 0;
  u64 snapshot_visible_through_transaction_id = 0;
  bool catalog_evidence_written = false;
  bool physical_build_complete = false;
  bool validation_complete = false;
  bool statistics_refresh_requested = false;
  bool optimizer_plan_invalidation_requested = false;
  bool crash_evidence_present = false;
  bool recovery_replay = false;
  bool checkpoint_reachable = true;
};

struct IndexRecoveryEvidence {
  IndexStatisticsLifecycleState durable_state = IndexStatisticsLifecycleState::absent;
  bool catalog_record_present = false;
  bool physical_root_present = false;
  bool build_manifest_complete = false;
  bool drop_tombstone_present = false;
  bool statistics_refresh_in_progress = false;
  bool checksum_valid = true;
  bool checkpoint_reachable = true;
  bool resource_epoch_current = true;
};

struct IndexStatisticsLifecycleResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool admitted = false;
  IndexLifecycleDescriptor descriptor;
  IndexStatisticsSnapshot statistics;
  bool statistics_refreshed = false;
  bool optimizer_plan_cache_invalidation_required = false;
  bool index_scan_allowed = false;
  bool stale_statistics_refused = false;
  IndexRecoveryClassification recovery_classification = IndexRecoveryClassification::clean_ready;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && admitted; }
};

const char* IndexStatisticsLifecycleStateName(IndexStatisticsLifecycleState state);
const char* IndexRecoveryClassificationName(IndexRecoveryClassification classification);

bool IndexResourceEpochVectorValid(const IndexResourceEpochVector& epochs);
bool IndexResourceEpochVectorEqual(const IndexResourceEpochVector& left,
                                   const IndexResourceEpochVector& right);
bool IndexStatisticsVisibleToSnapshot(const IndexStatisticsSnapshot& statistics,
                                      u64 snapshot_visible_through_transaction_id);

IndexStatisticsLifecycleResult ValidateCatalogPhysicalIndexProfileCoupling(
    const IndexLifecycleDescriptor& descriptor,
    const scratchbird::core::catalog::CatalogPhysicalIndexProfile* profile);
IndexStatisticsLifecycleResult ClassifyIndexLifecycleRecovery(
    const IndexLifecycleDescriptor& descriptor,
    const IndexRecoveryEvidence& evidence,
    u64 local_transaction_id);
IndexStatisticsLifecycleResult RefreshIndexStatistics(
    const IndexStatisticsLifecycleRequest& request);
IndexStatisticsLifecycleResult EvaluateIndexStatisticsForUse(
    const IndexLifecycleDescriptor& descriptor,
    const IndexStatisticsSnapshot& statistics,
    const IndexResourceEpochVector& runtime_epochs,
    IndexStatisticsFreshnessPolicy freshness_policy,
    u64 snapshot_visible_through_transaction_id);
IndexStatisticsLifecycleResult PlanIndexStatisticsLifecycle(
    const IndexStatisticsLifecycleRequest& request,
    const scratchbird::core::catalog::CatalogPhysicalIndexProfile* catalog_profile);

DiagnosticRecord MakeIndexStatisticsLifecycleDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail = {});

}  // namespace scratchbird::core::index
