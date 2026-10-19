// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"

namespace scratchbird::core::catalog {
enum class LockWaitBehavior : u64 {
  no_wait=1,wait_forever,wait_timeout,wait_until_statement_end,wait_until_transaction_end,
  wait_with_deadlock_detection,wait_with_priority,donor_compatible_wait,maintenance_window_wait
};
enum class LockQueueFairness : u64 { fifo=1,priority_with_starvation_prevention };
enum class LockVictimPolicy : u64 { core_deterministic_logical_lock_order=1 };
enum class LockVictimOutcome : u64 { statement_rollback=1,transaction_rollback,operation_pause,operation_retry,administrator_review,cluster_recovery_required,donor_compatible_error };
enum class LockTimeoutOutcome : u64 { lock_timeout_error=1,donor_compatible_error,retry_required,administrator_review,maintenance_deferred };
enum class LockPredicateGuard : u64 { tracked_validation_sets=1,conservative_blocking };
inline constexpr u64 kLockPolicyMandatoryFlags=31;
// Complete policy declarations, not a lock grant, queue, admitted memory or
// actual wait/deadlock decision. The owner must enforce the selected behavior.
struct CatalogLockPolicyBody {
  Uuid body_uuid,database_uuid;
  u64 generation=0;
  LockWaitBehavior wait_behavior=LockWaitBehavior::no_wait;
  u64 wait_timeout_microseconds=0;
  LockQueueFairness fairness=LockQueueFairness::fifo;
  u64 fairness_threshold_microseconds=0;
  u64 maximum_locks=0,maximum_waiters=0,maximum_lock_memory_bytes=0;
  u64 maximum_graph_nodes=0,maximum_graph_edges=0,maximum_deadlock_work=0;
  u64 deadlock_detection_interval_microseconds=0;
  LockVictimPolicy victim_policy=LockVictimPolicy::core_deterministic_logical_lock_order;
  LockVictimOutcome victim_outcome=LockVictimOutcome::statement_rollback;
  LockTimeoutOutcome timeout_outcome=LockTimeoutOutcome::lock_timeout_error;
  bool conversion_priority=false;
  u64 escalation_lock_threshold=0,escalation_memory_threshold=0;
  LockPredicateGuard predicate_guard=LockPredicateGuard::tracked_validation_sets;
  Uuid evidence_policy_uuid;
  std::optional<Uuid> donor_mapping_uuid,maintenance_window_policy_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id=0;
  TypedUuid creating_transaction_uuid;
  u64 creating_local_transaction_id=0;
  std::optional<Uuid> cluster_uuid;
  u64 flags=kLockPolicyMandatoryFlags;
};
inline constexpr u32 kCatalogLockPolicySchemaId=65626;
inline constexpr std::size_t kCatalogLockPolicyMaxBytes=568;
struct CatalogLockPolicyResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogLockPolicyBody> record;
  bool ok()const noexcept{return error==CatalogValueError::none&&record.has_value();}
};
std::optional<std::size_t> CatalogLockPolicyEncodedBytes(const CatalogLockPolicyBody&) noexcept;
CatalogValueEncodeIntoResult EncodeCatalogLockPolicyInto(const CatalogLockPolicyBody&,std::span<byte>) noexcept;
CatalogLockPolicyResult DecodeCatalogLockPolicy(std::string_view) noexcept;
bool IsCatalogLockPolicyPayload(std::string_view) noexcept;
bool CatalogLockPolicyMatchesHeader(const CatalogTypedRecordView&) noexcept;
bool CatalogLockPolicyMatchesMetadata(const CatalogMetadataVersionView&) noexcept;
bool CatalogLockPolicyMatchesDatabase(const CatalogMetadataVersionView&,const Uuid&) noexcept;
bool CatalogLockPolicyPreservesOrigin(const CatalogMetadataVersionView&,const CatalogMetadataVersionView&) noexcept;
bool CatalogLockPolicyMatchesDescriptor(const CatalogLockPolicyBody&,const CatalogPolicyDescriptor&) noexcept;
} // namespace scratchbird::core::catalog
