// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PARALLEL-PHYSICAL-STORAGE-ROUTES-ANCHOR
#include <string>
#include <vector>

namespace scratchbird::storage::page {

enum class ParallelStoragePipelineRouteFamily {
  kPageScan,
  kPageSummaryPrune,
  kIndexBuild
};

struct ParallelStoragePipelineRouteEvidence {
  bool supported = false;
  std::string worker_route_descriptor;
  std::vector<std::string> evidence;
};

struct ParallelStoragePipelineWorkerRouteDescriptor {
  bool supported = false;
  std::string family_name;
  std::string worker_route_descriptor;
  std::vector<std::string> evidence;
};

const char* ParallelStoragePipelineRouteFamilyName(
    ParallelStoragePipelineRouteFamily family);

ParallelStoragePipelineRouteEvidence BuildParallelStoragePipelineRouteEvidence(
    ParallelStoragePipelineRouteFamily family);

ParallelStoragePipelineWorkerRouteDescriptor
BuildParallelStoragePipelineWorkerRouteDescriptor(
    ParallelStoragePipelineRouteFamily family,
    unsigned worker_id,
    std::string snapshot_token_id);

}  // namespace scratchbird::storage::page
