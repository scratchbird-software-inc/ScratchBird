// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_relation_store.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_STORE_INTERNAL_VISIBILITY_BRIDGE
// Read-only bridge for authority-owned store modules. The durable transaction
// inventory remains the visibility/finality authority; consumers receive only
// its projected relation-read state.
EngineApiDiagnostic OverlayMgaTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    RelationReadSnapshot* state,
    bool allow_read_only_active);

// Narrow mutation-authority bridge for decomposed persistence modules. This
// validates the request against the durable transaction inventory; it does not
// publish, commit, roll back, or otherwise alter transaction state.
EngineApiDiagnostic ValidateMgaMutatingTransactionAuthorityForStoreModule(
    const EngineRequestContext& context,
    const std::string& operation_id);

}  // namespace scratchbird::engine::internal_api
