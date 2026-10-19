// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include "catalog_record_codec.hpp"

namespace scratchbird::core::catalog {
enum class IsolationProfileKind : u64 {
  read_uncommitted_emulated=1,read_committed_statement,read_committed_cursor_stable,
  snapshot_stable,repeatable_read_snapshot,serializable_snapshot,serializable_validated,
  read_only_historical_snapshot,internal_recovery_isolation,maintenance_operation_isolation,
  cluster_participant_isolation
};
enum class IsolationSnapshotPoint : u64 {
  transaction_start=1,statement_start,cursor_open,fetch,operation_start,historical_boundary,cluster_participant_start
};
enum class IsolationOwnWrites : u64 { visible_until_savepoint_rollback=1,hidden,profile_specific };
enum class IsolationActiveOthers : u64 { never_visible=1,diagnostic_only,compatibility_view_only };
enum class IsolationLimbo : u64 { ordinary_hidden=1,admin_visible,policy_wait,recovery_visible };
enum class IsolationPredicateValidation : u64 { none=1,declared_sets,conservative_blocking,serializable_required };
enum class IsolationUniqueValidation : u64 { visible_committed_plus_own=1,visible_plus_active_wait,cluster_authority,donor_profile };
enum class IsolationMetadataVisibility : u64 { transaction_epoch=1,statement_epoch,cursor_epoch,cluster_epoch,historical_epoch };
enum class IsolationClusterMode : u64 { not_clustered=1,local_until_cluster_write,cluster_bound,cluster_authority };
// Persisted declarations. Resolving this record never acquires a snapshot,
// locks, internal-service rights, a historical pin or cluster authority.
struct CatalogIsolationProfile {
  Uuid profile_uuid,database_uuid;
  u64 generation=0;
  IsolationProfileKind profile_kind=IsolationProfileKind(0);
  IsolationSnapshotPoint snapshot_construction_point=IsolationSnapshotPoint(0);
  // Closed bits: never, statement_start, statement_end, cursor_fetch,
  // savepoint_release, explicit_refresh, cluster_epoch_change. Never is exclusive.
  u64 snapshot_refresh_mask=0;
  IsolationOwnWrites own_write_visibility=IsolationOwnWrites::visible_until_savepoint_rollback;
  IsolationActiveOthers active_other_visibility=IsolationActiveOthers::never_visible;
  IsolationLimbo limbo_visibility=IsolationLimbo::ordinary_hidden;
  // Closed bits: mutation_admission, before_version_create, statement_end,
  // prepare, commit_validation. At least one is required.
  u64 write_conflict_detection_mask=0;
  Uuid wait_policy_uuid,deadlock_policy_uuid;
  IsolationPredicateValidation predicate_validation=IsolationPredicateValidation::none;
  IsolationUniqueValidation unique_validation=IsolationUniqueValidation::visible_committed_plus_own;
  IsolationMetadataVisibility metadata_visibility=IsolationMetadataVisibility(0);
  IsolationClusterMode cluster_participant_mode=IsolationClusterMode::not_clustered;
  std::optional<Uuid> donor_mapping_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
  std::optional<Uuid> cluster_uuid;
};
inline constexpr u32 kCatalogIsolationProfileSchemaId=65625;
inline constexpr std::size_t kCatalogIsolationProfileMaxBytes=440;
struct CatalogIsolationProfileResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogIsolationProfile> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogIsolationProfileEncodedBytes(const CatalogIsolationProfile&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogIsolationProfileInto(const CatalogIsolationProfile&,std::span<byte>) noexcept;
CatalogIsolationProfileResult DecodeCatalogIsolationProfile(std::string_view) noexcept;
bool IsCatalogIsolationProfilePayload(std::string_view) noexcept;
bool CatalogIsolationProfileMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogIsolationProfileMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogIsolationProfileMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogIsolationProfilePreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
} // namespace scratchbird::core::catalog
