// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_TRANSACTION_TRANSACTION_API
struct EngineBeginTransactionRequest : EngineApiRequest {
  std::string isolation_level;
  EngineProfileSet transaction_policy_profile;
};
struct EngineBeginTransactionResult : EngineApiResult {
  EngineUuid transaction_uuid;
  EngineApiU64 local_transaction_id = 0;
  std::string isolation_level;
  std::string read_mode = "read_write";
  bool read_only = false;
  EngineApiU64 snapshot_visible_through_local_transaction_id = 0;
};
EngineBeginTransactionResult EngineBeginTransaction(const EngineBeginTransactionRequest& request);

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

struct EngineRollbackTransactionRequest : EngineApiRequest {};
struct EngineRollbackTransactionResult : EngineApiResult {};
EngineRollbackTransactionResult EngineRollbackTransaction(const EngineRollbackTransactionRequest& request);

struct EngineCleanupTemporarySessionRequest : EngineApiRequest {};
struct EngineCleanupTemporarySessionResult : EngineApiResult {
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

struct EngineExecuteTransactionBlockRequest : EngineApiRequest {};
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
