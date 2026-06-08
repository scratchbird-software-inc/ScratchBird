// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "index_apply_planner.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <vector>

namespace scratchbird::engine::internal_api {

struct LocalityAwareIndexApplyBatchPlan {
  EngineApiDiagnostic diagnostic;
  std::vector<MgaIndexEntryAppendBatch> batches;
  scratchbird::core::index::CommitGroupLocalityIndexApplyPlan core_plan;
};

LocalityAwareIndexApplyBatchPlan PlanLocalityAwareIndexApplyBatches(
    const std::vector<MgaIndexEntryAppendBatch>& batches);

void AddLocalityAwareIndexApplyEvidence(
    const LocalityAwareIndexApplyBatchPlan& plan,
    std::vector<EngineEvidenceReference>* evidence);

}  // namespace scratchbird::engine::internal_api
