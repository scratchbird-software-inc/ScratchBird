// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_physical_storage_routes.hpp"

#include <utility>

namespace scratchbird::storage::page {

const char* ParallelStoragePipelineRouteFamilyName(
    ParallelStoragePipelineRouteFamily family) {
  switch (family) {
    case ParallelStoragePipelineRouteFamily::kPageScan:
      return "page_scan";
    case ParallelStoragePipelineRouteFamily::kPageSummaryPrune:
      return "page_summary_prune";
    case ParallelStoragePipelineRouteFamily::kIndexBuild:
      return "index_build";
  }
  return "unknown";
}

ParallelStoragePipelineRouteEvidence BuildParallelStoragePipelineRouteEvidence(
    ParallelStoragePipelineRouteFamily family) {
  ParallelStoragePipelineRouteEvidence route;
  route.supported = true;
  const std::string family_name =
      ParallelStoragePipelineRouteFamilyName(family);
  route.worker_route_descriptor =
      "storage.page.parallel_worker." + family_name;
  route.evidence.push_back("storage.parallel_pipeline.family=" + family_name);
  route.evidence.push_back("storage.parallel_pipeline.worker_route_descriptor=" +
                           route.worker_route_descriptor);
  route.evidence.push_back(
      "storage.parallel_pipeline.snapshot_token_required=true");
  route.evidence.push_back(
      "storage.parallel_pipeline.candidate_fragments_only=true");
  route.evidence.push_back(
      "storage.parallel_pipeline.finality_delegated_to_executor_merge=true");
  route.evidence.push_back(
      "storage.parallel_pipeline.page_visibility_recheck_delegated=true");
  return route;
}

ParallelStoragePipelineWorkerRouteDescriptor
BuildParallelStoragePipelineWorkerRouteDescriptor(
    ParallelStoragePipelineRouteFamily family,
    unsigned worker_id,
    std::string snapshot_token_id) {
  ParallelStoragePipelineWorkerRouteDescriptor descriptor;
  descriptor.supported = true;
  descriptor.family_name = ParallelStoragePipelineRouteFamilyName(family);
  descriptor.worker_route_descriptor =
      "storage.page.parallel_worker." + descriptor.family_name;
  descriptor.evidence.push_back("storage.parallel_worker.worker_id=" +
                                std::to_string(worker_id));
  descriptor.evidence.push_back("storage.parallel_worker.family=" +
                                descriptor.family_name);
  descriptor.evidence.push_back("storage.parallel_worker.snapshot_token=" +
                                std::move(snapshot_token_id));
  descriptor.evidence.push_back(
      "storage.parallel_worker.route_descriptor=" +
      descriptor.worker_route_descriptor);
  descriptor.evidence.push_back(
      "storage.parallel_worker.produces_candidate_fragments_only=true");
  descriptor.evidence.push_back(
      "storage.parallel_worker.finality_delegated_to_executor_merge=true");
  return descriptor;
}

}  // namespace scratchbird::storage::page
