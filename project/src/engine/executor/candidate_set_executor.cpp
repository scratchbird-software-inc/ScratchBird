// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set_executor.hpp"

namespace scratchbird::engine::executor {

ExecutorCandidateSetFinalizeResult FinalizeCandidateSetForExecutor(
    const scratchbird::core::index::CandidateSet& candidates,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority) {
  ExecutorCandidateSetFinalizeResult result;
  result.recheck =
      scratchbird::core::index::ExactRecheckCandidateSet(candidates, authority);
  result.status = result.recheck.status;
  result.fail_closed = result.recheck.fail_closed || !result.recheck.ok();
  result.evidence = result.recheck.evidence;
  result.evidence.push_back("executor.final_result_requires_exact_row_uuid=true");
  result.evidence.push_back("executor.final_result_requires_mga_recheck=true");
  result.evidence.push_back("executor.final_result_requires_security_recheck=true");
  if (!result.fail_closed) {
    for (const auto& row : result.recheck.output.rows) {
      result.final_row_uuids.push_back(row.row_uuid);
    }
  }
  return result;
}

}  // namespace scratchbird::engine::executor
