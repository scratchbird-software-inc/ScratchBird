// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

#include <functional>
#include <map>

namespace scratchbird::engine::internal_api {

struct ScopedRelationSummaryDelta {
  std::uint64_t row_version_count = 0;
  std::uint64_t tombstone_count = 0;
  std::uint64_t update_count = 0;
  bool first_scoped_write = false;
};

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_STORE_INTERNAL_VISIBILITY_BRIDGE
// Read-only bridge for authority-owned store modules. The durable transaction
// inventory remains the visibility/finality authority; consumers receive only
// its projected relation-read state.
EngineApiDiagnostic OverlayMgaTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    RelationReadSnapshot* state,
    bool allow_read_only_active);

// Narrow authority bridges for decomposed persistence modules. They validate
// or project durable transaction/savepoint state; none publish, commit, roll
// back, or otherwise alter transaction state.
EngineApiDiagnostic ValidateMgaMutatingTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    const std::string& operation_id);

std::function<bool(std::uint64_t, std::uint64_t)>
MakeMgaMetadataRollbackPredicateForStoreModule(
    const EngineRequestContext& context);
bool ExactTextMigrationCreatorTransactionForStoreModule(
    const EngineRequestContext& context,
    std::uint64_t creator_tx,
    std::string_view transaction_uuid);
bool TextMigrationLineageCreatorVisibleForStoreModule(
    const EngineRequestContext& context,
    std::uint64_t migration_creator_tx,
    std::uint64_t candidate_creator_tx);

bool UpdateScopedRelationSummariesForStoreModule(
    const EngineRequestContext& context,
    const std::map<std::string, ScopedRelationSummaryDelta>& deltas);

}  // namespace scratchbird::engine::internal_api
