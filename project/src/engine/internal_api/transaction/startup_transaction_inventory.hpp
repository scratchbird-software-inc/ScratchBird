// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "transaction_inventory.hpp"

#include <optional>

namespace scratchbird::engine::internal_api {

// Read-only prerequisite for engine-owned startup reconciliation. Neither the
// request nor the observation proves startup operation/session ownership. The
// owning recovery service must retain that binding before effects, reconcile
// the complete operation, and publish its own matching recovery disposition.
struct StartupTransactionInventoryRequest {
  std::string database_path;
  EngineUuid database_uuid;
  EngineUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

enum class StartupTransactionInventoryOutcome : std::uint8_t {
  invalid_request,
  authority_unavailable,
  database_mismatch,
  identity_missing,
  identity_mismatch,
  identity_ambiguous,
  unresolved,
  committed,
  rolled_back,
};

struct StartupTransactionInventoryObservation {
  StartupTransactionInventoryOutcome outcome =
      StartupTransactionInventoryOutcome::authority_unavailable;
  // Present only after strong native loading. This is inventory publication
  // provenance, NOT a startup recovery generation or restart/admission grant.
  std::optional<scratchbird::transaction::mga::TransactionInventoryPublicationBase>
      publication_base;
  scratchbird::transaction::mga::TransactionState observed_state =
      scratchbird::transaction::mga::TransactionState::none;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
};

// The routed engine owner must already hold cross-process database ownership.
// Serializes with engine inventory publication and performs a strong native
// read, never trusts a retained caller observation or a publication receipt.
// Missing entries are unknown effects, not proof that work never happened.
// A terminal result is an observation at this read only. It neither mutates
// inventory nor clears a fence, and cannot be used as a bearer recovery token.
StartupTransactionInventoryObservation InspectStartupTransactionInventory(
    const StartupTransactionInventoryRequest& request);

}  // namespace scratchbird::engine::internal_api
