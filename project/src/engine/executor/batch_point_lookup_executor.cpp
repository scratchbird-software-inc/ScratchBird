// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "batch_point_lookup_executor.hpp"

namespace scratchbird::engine::executor {

ExecutorBatchPointLookupResult ExecuteBatchPointLookupForExecutor(
    const scratchbird::core::index::BatchPointLookupPlan& plan,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority,
    const scratchbird::core::index::BatchPointLookupProvider& provider) {
  ExecutorBatchPointLookupResult result;
  result.lookup =
      scratchbird::core::index::RunBatchPointLookup(plan, authority, provider);
  result.status = result.lookup.status;
  result.fail_closed = result.lookup.fail_closed || !result.lookup.ok() ||
                       !result.lookup.final_rows_authorized;
  result.evidence = result.lookup.evidence;
  result.evidence.push_back(
      "executor.batch_point_lookup.requires_exact_row_uuid=true");
  result.evidence.push_back(
      "executor.batch_point_lookup.requires_mga_recheck=true");
  result.evidence.push_back(
      "executor.batch_point_lookup.requires_security_recheck=true");
  result.evidence.push_back(
      "executor.batch_point_lookup.transaction_finality_authority=false");
  if (!result.fail_closed) {
    result.rows = result.lookup.rows;
    result.misses = result.lookup.misses;
  }
  return result;
}

}  // namespace scratchbird::engine::executor
