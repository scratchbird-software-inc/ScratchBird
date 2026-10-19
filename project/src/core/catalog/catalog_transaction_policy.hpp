// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"
#include <array>

namespace scratchbird::core::catalog {
enum class TransactionSavepointMode : u64 { disabled=1,engine_transaction_local };
enum class TransactionAutocommitMode : u64 { explicit_transaction=1,statement_transaction };
enum class TransactionClusterEligibility : u64 { local_only=1,require_cluster_admission };
enum class TransactionRetentionBehavior : u64 { pin_required_horizons_until_retirement=1 };
enum class TransactionLimboBehavior : u64 { block_and_require_recovery=1 };
enum class TransactionCommitOrdering : u64 { engine_commit_sequence=1 };
enum class TransactionRestartBehavior : u64 { no_automatic_restart=1,bounded_statement_restart };
inline constexpr u64 kTransactionPolicyMandatoryFlags=1023;
// Complete declarations only. The owner resolves the actual isolation, lock
// and evidence policies and enforces admission before publishing active MGA.
struct CatalogTransactionPolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  Uuid default_isolation_profile_uuid,startup_isolation_profile_uuid;
  std::array<Uuid,64> allowed_isolation_profiles{};
  std::size_t allowed_isolation_profile_count=0;
  u64 read_mode_mask=0,maximum_transaction_microseconds=0,maximum_snapshot_microseconds=0;
  u64 maximum_active_transactions=0,maximum_savepoints=0,maximum_prepared_transactions=0;
  Uuid lock_policy_uuid;
  u64 deadlock_priority=0;
  TransactionSavepointMode savepoint_mode=TransactionSavepointMode::disabled;
  TransactionAutocommitMode autocommit_mode=TransactionAutocommitMode::explicit_transaction;
  TransactionClusterEligibility cluster_eligibility=TransactionClusterEligibility::local_only;
  TransactionRetentionBehavior retention_behavior=TransactionRetentionBehavior::pin_required_horizons_until_retirement;
  u64 reload_trigger_mask=15;
  TransactionLimboBehavior limbo_behavior=TransactionLimboBehavior::block_and_require_recovery;
  TransactionCommitOrdering commit_ordering=TransactionCommitOrdering::engine_commit_sequence;
  TransactionRestartBehavior restart_behavior=TransactionRestartBehavior::no_automatic_restart;
  u64 maximum_statement_restarts=0;
  Uuid evidence_policy_uuid;
  u64 flags=kTransactionPolicyMandatoryFlags;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
  std::optional<Uuid> cluster_uuid;
};
inline constexpr u32 kCatalogTransactionPolicySchemaId=65624;
inline constexpr std::size_t kCatalogTransactionPolicyMaxBytes=1592;
struct CatalogTransactionPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogTransactionPolicyBody> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogTransactionPolicyEncodedBytes(const CatalogTransactionPolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogTransactionPolicyInto(const CatalogTransactionPolicyBody&,std::span<byte>) noexcept;
CatalogTransactionPolicyResult DecodeCatalogTransactionPolicy(std::string_view) noexcept;
bool IsCatalogTransactionPolicyPayload(std::string_view) noexcept;
bool CatalogTransactionPolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogTransactionPolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogTransactionPolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogTransactionPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
bool CatalogTransactionPolicyMatchesDescriptor(const CatalogTransactionPolicyBody&,const CatalogPolicyDescriptor&) noexcept;
} // namespace scratchbird::core::catalog
