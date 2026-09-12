// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "procedural/procedural_block_ir.hpp"
#include "transaction_snapshot.hpp"

#include <mutex>
#include <array>
#include <cstdint>

namespace scratchbird::engine::internal_api {

// Engine-internal publication ordering for one routed database. Transaction
// finality paths hold this guard across durable inventory replacement; private
// server-to-engine metadata dispatch holds the same guard from current-version
// validation through exact invocation. Cross-process exclusion is supplied by
// the routed server's database-owner lock, not by a public or validation-only
// engine handle.
std::unique_lock<std::recursive_mutex> AcquireTransactionInventoryGuard(
    const std::string& database_path);

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_TRANSACTION_TRANSACTION_API
enum class EngineTransactionInventoryState : std::uint8_t {
  not_started, active, committed, rolled_back, not_applied, unknown,
};

// Engine-issued observation of a composite inventory identity. This is not a
// bearer token or permission to execute: recovery must revalidate the exact
// database/session and UUID/local-id pair against current engine authority.
// In particular an attempted publication with unknown outcome is never active
// authority merely because a nonzero candidate identity is retained here.
// not_applied means the attempted commit/rollback did not apply; it does not
// assert that a transaction is currently active (an inventory read may fail).
struct EngineTransactionInventoryObservation {
  std::array<std::uint8_t, 16> transaction_uuid{};
  EngineApiU64 local_transaction_id = 0;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_timestamp;
  EngineTransactionInventoryState state = EngineTransactionInventoryState::not_started;
  bool post_inventory_secondary_failure = false;
};

struct EngineBeginTransactionRequest : EngineApiRequest {
  std::string isolation_level;
  EngineProfileSet transaction_policy_profile;
};
struct EngineBeginTransactionResult : EngineApiResult {
  EngineTransactionInventoryObservation inventory_observation;
  EngineUuid transaction_uuid;
  EngineApiU64 local_transaction_id = 0;
  std::string isolation_level;
  std::string read_mode = "read_write";
  bool read_only = false;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
};
EngineBeginTransactionResult EngineBeginTransaction(const EngineBeginTransactionRequest& request);

struct EnginePublishStatementSnapshotRequest : EngineApiRequest {};
struct EnginePublishStatementSnapshotResult : EngineApiResult {
  EngineUuid statement_uuid;
  EngineUuid statement_snapshot_uuid;
  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
};
EnginePublishStatementSnapshotResult EnginePublishStatementSnapshot(
    const EnginePublishStatementSnapshotRequest& request);

struct EngineResolveStatementSnapshotRequest : EngineApiRequest {};
struct EngineResolveStatementSnapshotResult : EngineApiResult {
  EngineUuid statement_uuid;
  EngineUuid statement_snapshot_uuid;
  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
};
EngineResolveStatementSnapshotResult EngineResolveStatementSnapshot(
    const EngineResolveStatementSnapshotRequest& request);

struct EngineSetTransactionCharacteristicsRequest : EngineApiRequest {};
struct EngineSetTransactionCharacteristicsResult : EngineApiResult {};
EngineSetTransactionCharacteristicsResult EngineSetTransactionCharacteristics(
    const EngineSetTransactionCharacteristicsRequest& request);

struct EngineCommitTransactionRequest : EngineApiRequest {};
struct EngineCommitTransactionResult : EngineApiResult {
  std::string commit_finality_state = "not_final";
  bool engine_finality_known = false;
  bool post_inventory_secondary_failure = false;
};
EngineCommitTransactionResult EngineCommitTransaction(const EngineCommitTransactionRequest& request);

struct EngineAutocommitBoundaryRequest : EngineApiRequest {
  bool statement_succeeded = true;
  std::string replacement_isolation_level;
  EngineProfileSet transaction_policy_profile;
};
struct EngineAutocommitBoundaryResult : EngineCommitTransactionResult {
  EngineUuid replacement_transaction_uuid;
  EngineApiU64 replacement_local_transaction_id = 0;
  EngineApiU64 replacement_snapshot_visible_through_local_transaction_id = 0;
  std::string replacement_transaction_timestamp;
  bool replacement_read_only = false;
  std::string replacement_read_mode = "read_write";
  std::string replacement_isolation_level;
};
EngineAutocommitBoundaryResult EngineAutocommitBoundary(
    const EngineAutocommitBoundaryRequest& request);

struct EngineRollbackTransactionRequest : EngineApiRequest {};
struct EngineRollbackTransactionResult : EngineApiResult {
  std::string rollback_finality_state = "not_final";
  bool engine_finality_known = false;
  bool post_inventory_secondary_failure = false;
};
EngineRollbackTransactionResult EngineRollbackTransaction(const EngineRollbackTransactionRequest& request);

struct EngineCleanupTemporarySessionRequest : EngineApiRequest {};
struct EngineCleanupTemporarySessionResult : EngineApiResult {
  EngineTransactionInventoryObservation cleanup_transaction;
  EngineApiU64 temporary_deleted_rows = 0;
  EngineApiU64 temporary_reclaimed_large_values = 0;
  EngineApiU64 temporary_retired_private_metadata = 0;
  EngineApiU64 cleanup_local_transaction_id = 0;
};
EngineCleanupTemporarySessionResult EngineCleanupTemporarySessionState(
    const EngineCleanupTemporarySessionRequest& request);

struct EnginePrepareTransactionRequest : EngineApiRequest {};
struct EnginePrepareTransactionResult : EngineApiResult {};
EnginePrepareTransactionResult EnginePrepareTransaction(const EnginePrepareTransactionRequest& request);

struct EngineExecuteTransactionBlockRequest : EngineApiRequest {
  EngineProceduralBlockV1 procedural_block;
  EngineApiDiagnostic procedural_block_diagnostic;
  bool procedural_block_present = false;
  bool procedural_block_valid = false;
};
struct EngineExecuteTransactionBlockResult : EngineApiResult {};
EngineExecuteTransactionBlockResult EngineExecuteTransactionBlock(
    const EngineExecuteTransactionBlockRequest& request);

struct EngineLockTableRequest : EngineApiRequest {};
struct EngineLockTableResult : EngineApiResult {
  std::string lock_surface;
  std::string lock_mode;
  std::string lock_policy;
  bool compatibility_noop = false;
  bool admission_fence = false;
};
EngineLockTableResult EngineLockTable(const EngineLockTableRequest& request);

struct EngineUnlockTableRequest : EngineApiRequest {};
struct EngineUnlockTableResult : EngineApiResult {
  std::string lock_surface;
  std::string release_outcome;
  bool compatibility_noop = false;
};
EngineUnlockTableResult EngineUnlockTable(const EngineUnlockTableRequest& request);

struct EngineLockNamedRequest : EngineApiRequest {};
struct EngineLockNamedResult : EngineApiResult {
  std::string lock_surface;
  std::string lock_decision;
  std::string resource_key;
  bool acquired = false;
};
EngineLockNamedResult EngineLockNamed(const EngineLockNamedRequest& request);

struct EngineUnlockNamedRequest : EngineApiRequest {};
struct EngineUnlockNamedResult : EngineApiResult {
  std::string lock_surface;
  std::string release_outcome;
  std::string resource_key;
  bool released = false;
};
EngineUnlockNamedResult EngineUnlockNamed(const EngineUnlockNamedRequest& request);

}  // namespace scratchbird::engine::internal_api
