// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PARALLEL-COPY-PIPELINE-ROUTE-ANCHOR
#include <string>
#include <vector>

namespace scratchbird::core::bulk_load {

struct ParallelCopyPipelineRouteEvidence {
  bool supported = false;
  std::string worker_route_descriptor;
  std::vector<std::string> evidence;
};

struct ParallelCopyPipelineWorkerRouteDescriptor {
  bool supported = false;
  std::string worker_route_descriptor;
  std::vector<std::string> evidence;
};

ParallelCopyPipelineRouteEvidence BuildParallelCopyPipelineRouteEvidence();

ParallelCopyPipelineWorkerRouteDescriptor
BuildParallelCopyPipelineWorkerRouteDescriptor(unsigned worker_id,
                                               std::string snapshot_token_id);

}  // namespace scratchbird::core::bulk_load
