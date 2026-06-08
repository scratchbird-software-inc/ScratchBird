// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-OWNERSHIP-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

#include <string>
#include <string_view>

namespace scratchbird::core::index {

enum class IndexSubsystemOwner : u32 { index_manager, memory_manager, page_agent, filespace_agent, optimizer, transaction_manager, security_manager, metrics_registry, archive_manager, parser_sblr, udr_registry, management_api };

struct IndexOwnershipDecision {
  Status status;
  bool allowed = false;
  IndexSubsystemOwner owner = IndexSubsystemOwner::index_manager;
  DiagnosticRecord diagnostic;
};

const char* IndexSubsystemOwnerName(IndexSubsystemOwner owner);
IndexOwnershipDecision EvaluateIndexOwnership(std::string_view action, IndexSubsystemOwner caller);

}  // namespace scratchbird::core::index
