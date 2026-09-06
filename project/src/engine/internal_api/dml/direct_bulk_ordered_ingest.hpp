// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "crud_support/crud_store.hpp"
#include "insert_physical_integration.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// Owns placement-order selection and physical-clustering policy resolution.
// It only reorders staged input; publication remains coordinator-owned.

struct DirectOrderedIngestSelection {
  bool ok = true;
  bool selected = false;
  EngineApiDiagnostic diagnostic;
  std::string failure_reason;
  std::vector<EngineEvidenceReference> evidence;
};

bool DirectOrderedIngestRequested(
    const DirectPhysicalBulkAppendRequest& request);

bool DirectPhysicalClusteringRequested(
    const DirectPhysicalBulkAppendRequest& request);

DirectOrderedIngestSelection ApplyDirectOrderedIngestPlan(
    const DirectPhysicalBulkAppendRequest& request,
    std::vector<CrudRowVersionRecord>* staged_rows,
    std::vector<std::vector<std::pair<std::string, std::string>>>*
        logical_value_batch);

}  // namespace scratchbird::engine::internal_api::dml::detail
