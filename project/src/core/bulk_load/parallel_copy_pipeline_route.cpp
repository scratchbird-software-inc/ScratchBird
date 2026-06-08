// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_copy_pipeline_route.hpp"

#include <utility>

namespace scratchbird::core::bulk_load {

ParallelCopyPipelineRouteEvidence BuildParallelCopyPipelineRouteEvidence() {
  ParallelCopyPipelineRouteEvidence route;
  route.supported = true;
  route.worker_route_descriptor =
      "core.bulk_load.parallel_worker.copy_decode_bind_append";
  route.evidence.push_back(
      "bulk.parallel_pipeline.family=copy_decode_bind_append");
  route.evidence.push_back("bulk.parallel_pipeline.worker_route_descriptor=" +
                           route.worker_route_descriptor);
  route.evidence.push_back(
      "bulk.parallel_pipeline.snapshot_token_required=true");
  route.evidence.push_back(
      "bulk.parallel_pipeline.decode_bind_append_fragments_only=true");
  route.evidence.push_back(
      "bulk.parallel_pipeline.publication_delegated_to_executor_merge=true");
  route.evidence.push_back(
      "bulk.parallel_pipeline.strict_bulk_lifecycle_finality_required=true");
  return route;
}

ParallelCopyPipelineWorkerRouteDescriptor
BuildParallelCopyPipelineWorkerRouteDescriptor(unsigned worker_id,
                                               std::string snapshot_token_id) {
  ParallelCopyPipelineWorkerRouteDescriptor descriptor;
  descriptor.supported = true;
  descriptor.worker_route_descriptor =
      "core.bulk_load.parallel_worker.copy_decode_bind_append";
  descriptor.evidence.push_back("bulk.parallel_worker.worker_id=" +
                                std::to_string(worker_id));
  descriptor.evidence.push_back(
      "bulk.parallel_worker.family=copy_decode_bind_append");
  descriptor.evidence.push_back("bulk.parallel_worker.snapshot_token=" +
                                std::move(snapshot_token_id));
  descriptor.evidence.push_back("bulk.parallel_worker.route_descriptor=" +
                                descriptor.worker_route_descriptor);
  descriptor.evidence.push_back(
      "bulk.parallel_worker.decode_bind_append_fragments_only=true");
  descriptor.evidence.push_back(
      "bulk.parallel_worker.publication_delegated_to_executor_merge=true");
  return descriptor;
}

}  // namespace scratchbird::core::bulk_load
