// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "transaction_inventory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_MGA_LOCAL_COMMIT_PAGE_BARRIER
//
// This manifest is durable classification evidence for the transaction's
// local publication barrier.  It is deliberately not a redo/undo log and is
// never transaction-finality authority.  Finality remains exclusively in the
// durable MGA transaction inventory.
struct LocalCommitPublicationArtifact {
  std::string mutation_domain;
  std::string artifact_identity;
  std::uint64_t durable_size_bytes{0};
  std::string postcondition_sha256;
};

struct LocalCommitPublicationMutation {
  std::string mutation_identity;
  std::string mutation_domain;
  std::string mutation_kind;
  EngineUuid object_identity;
  EngineUuid record_identity;
  EngineUuid version_identity;
  std::string physical_identity;
  std::uint64_t generation_before{0};
  std::uint64_t generation_after{0};
  std::string idempotency_key;
  std::string precondition_sha256;
  std::string postcondition_sha256;
  std::string finality_authority{"durable_transaction_inventory"};
  std::string lifecycle_state{"commit_publish_ready"};
};

struct LocalCommitPublicationResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  std::string manifest_path;
  std::string manifest_sha256;
  std::uint64_t publication_generation{0};
  std::vector<LocalCommitPublicationMutation> mutations;
  std::vector<LocalCommitPublicationArtifact> artifacts;
};

enum class LocalCommitPublicationRecoveryClass {
  retryable_unpublished,
  committed_by_inventory,
  abandoned_by_rollback,
  in_doubt,
  corrupt_manifest,
};

struct LocalCommitPublicationRecoveryResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  std::string manifest_sha256;
  LocalCommitPublicationRecoveryClass recovery_class{
      LocalCommitPublicationRecoveryClass::corrupt_manifest};
  std::string stable_reason;
  std::uint64_t publication_generation{0};
  std::vector<LocalCommitPublicationMutation> mutations;
  std::vector<LocalCommitPublicationArtifact> artifacts;
};

// Classifies an admitted native entry, not a manifest's claimed finality. A
// failed or malformed outcome requires review; neither is rollback evidence.
inline LocalCommitPublicationRecoveryClass ClassifyLocalCommitPublicationInventoryOutcome(
    const scratchbird::transaction::mga::TransactionInventoryEntry& entry) {
  using scratchbird::transaction::mga::TransactionState;
  switch (scratchbird::transaction::mga::InventoryVisibilityState(entry)) {
    case TransactionState::committed:
      return LocalCommitPublicationRecoveryClass::committed_by_inventory;
    case TransactionState::rolled_back:
      return LocalCommitPublicationRecoveryClass::abandoned_by_rollback;
    case TransactionState::created:
    case TransactionState::active:
    case TransactionState::read_only_active:
    case TransactionState::preparing:
    case TransactionState::committing:
    case TransactionState::rolling_back:
      return LocalCommitPublicationRecoveryClass::retryable_unpublished;
    default:
      return LocalCommitPublicationRecoveryClass::in_doubt;
  }
}

const char* LocalCommitPublicationRecoveryClassName(
    LocalCommitPublicationRecoveryClass recovery_class);

LocalCommitPublicationResult RunLocalCommitPageBarrier(
    const EngineRequestContext& context);

LocalCommitPublicationRecoveryResult ClassifyLocalCommitPublicationForRecovery(
    const EngineRequestContext& context,
    const scratchbird::transaction::mga::LocalTransactionInventory& inventory);

}  // namespace scratchbird::engine::internal_api
