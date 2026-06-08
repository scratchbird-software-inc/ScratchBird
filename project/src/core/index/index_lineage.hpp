// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-LINEAGE-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

enum class IndexLineageBehavior : u32 {
  persist,
  rebuild,
  skip,
  transform,
  verify,
  refuse
};

struct IndexLineageDecision {
  Status status;
  IndexLineageBehavior archive_behavior = IndexLineageBehavior::verify;
  IndexLineageBehavior replication_behavior = IndexLineageBehavior::rebuild;
  IndexLineageBehavior restore_behavior = IndexLineageBehavior::verify;
  std::string verification_rule;
  DiagnosticRecord diagnostic;
};

const char* IndexLineageBehaviorName(IndexLineageBehavior behavior);
IndexLineageDecision DecideIndexLineage(IndexFamily family, bool resource_available, bool finality_proven);

}  // namespace scratchbird::core::index
