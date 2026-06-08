// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-BATCH-POINT-LOOKUP-ANCHOR
#include "batch_point_lookup.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct ExecutorBatchPointLookupResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  std::vector<scratchbird::core::index::BatchPointLookupRow> rows;
  std::vector<scratchbird::core::index::BatchPointLookupMiss> misses;
  scratchbird::core::index::BatchPointLookupResult lookup;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

ExecutorBatchPointLookupResult ExecuteBatchPointLookupForExecutor(
    const scratchbird::core::index::BatchPointLookupPlan& plan,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority,
    const scratchbird::core::index::BatchPointLookupProvider& provider);

}  // namespace scratchbird::engine::executor
