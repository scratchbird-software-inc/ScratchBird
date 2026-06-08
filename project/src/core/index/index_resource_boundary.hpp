// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-RESOURCE-BOUNDARY-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

#include <string_view>

namespace scratchbird::core::index {

enum class IndexResourceKind : u32 { collation, charset, tokenizer, analyzer, spatial, vector_metric, quantizer, trusted_udr, llvm_profile };

enum class IndexResourceAction : u32 { usable, mark_stale, rebuild_required, refuse };

struct IndexResourceDecision {
  Status status;
  IndexResourceAction action = IndexResourceAction::refuse;
  DiagnosticRecord diagnostic;
};

const char* IndexResourceKindName(IndexResourceKind kind);
const char* IndexResourceActionName(IndexResourceAction action);
IndexResourceDecision EvaluateIndexResource(IndexResourceKind kind, bool registered, bool epoch_current, bool deterministic);

}  // namespace scratchbird::core::index
