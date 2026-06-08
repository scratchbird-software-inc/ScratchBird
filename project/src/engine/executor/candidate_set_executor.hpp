// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-CANDIDATE-SET-FINALIZE-ANCHOR

#include "candidate_set.hpp"

#include <vector>

namespace scratchbird::engine::executor {

struct ExecutorCandidateSetFinalizeResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  std::vector<scratchbird::core::platform::TypedUuid> final_row_uuids;
  scratchbird::core::index::CandidateSetResult recheck;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

ExecutorCandidateSetFinalizeResult FinalizeCandidateSetForExecutor(
    const scratchbird::core::index::CandidateSet& candidates,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority);

}  // namespace scratchbird::engine::executor
