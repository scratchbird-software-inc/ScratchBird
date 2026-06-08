// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ordered_ingest.hpp"

namespace scratchbird::storage::page {
namespace {

void AddEvidence(OrderedIngestPhysicalClusteringResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

OrderedIngestPhysicalClusteringResult Refuse(
    const OrderedIngestPhysicalClusteringRequest& request,
    std::string diagnostic_code,
    std::string diagnostic_detail) {
  OrderedIngestPhysicalClusteringResult result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(diagnostic_code);
  result.diagnostic_detail = std::move(diagnostic_detail);
  result.descriptor = request.current_descriptor;
  AddEvidence(&result, "ordered_ingest_physical_clustering", "fail_closed");
  AddEvidence(&result,
              "ordered_ingest_clustering_diagnostic",
              result.diagnostic_detail);
  AddEvidence(&result,
              "ordered_ingest_clustering_key_change_allowed",
              request.allow_clustering_key_change ? "true" : "false");
  AddEvidence(&result,
              "ordered_ingest_clustering_policy_explicit",
              request.explicit_policy_present ? "true" : "false");
  return result;
}

}  // namespace

OrderedIngestPhysicalClusteringResult ResolveOrderedIngestPhysicalClustering(
    const OrderedIngestPhysicalClusteringRequest& request) {
  OrderedIngestPhysicalClusteringResult result;
  result.descriptor = request.current_descriptor;
  AddEvidence(&result, "ordered_ingest_storage_policy", "storage_page");
  AddEvidence(&result,
              "ordered_ingest_selected",
              request.ordered_ingest_selected ? "true" : "false");
  AddEvidence(&result,
              "ordered_ingest_physical_clustering_requested",
              request.physical_clustering_requested ? "true" : "false");

  if (!request.physical_clustering_requested) {
    AddEvidence(&result,
                "ordered_ingest_physical_clustering",
                "not_requested_descriptor_unchanged");
    return result;
  }

  if (!request.explicit_policy_present) {
    return Refuse(request,
                  "SB-STORAGE-ORDERED-INGEST-CLUSTERING-POLICY-REQUIRED",
                  "physical_clustering_policy_required");
  }
  if (request.requested_placement_key_column.empty()) {
    return Refuse(request,
                  "SB-STORAGE-ORDERED-INGEST-CLUSTERING-KEY-REQUIRED",
                  "physical_clustering_key_required");
  }

  const std::string& current_key = request.current_descriptor.placement_key_column;
  const bool key_change =
      !current_key.empty() && current_key != request.requested_placement_key_column;
  if (key_change && !request.allow_clustering_key_change) {
    return Refuse(request,
                  "SB-STORAGE-ORDERED-INGEST-CLUSTERING-KEY-CHANGE-REFUSED",
                  "physical_clustering_key_change_requires_explicit_policy");
  }

  result.descriptor.physical_clustering_enabled = true;
  result.descriptor.placement_key_column = request.requested_placement_key_column;
  if (!request.requested_policy_uuid.empty()) {
    result.descriptor.policy_uuid = request.requested_policy_uuid;
  }
  if (result.descriptor.descriptor_generation == 0) {
    result.descriptor.descriptor_generation = 1;
  } else if (current_key != result.descriptor.placement_key_column ||
             request.current_descriptor.policy_uuid != result.descriptor.policy_uuid ||
             !request.current_descriptor.physical_clustering_enabled) {
    ++result.descriptor.descriptor_generation;
  }
  result.descriptor_updated =
      result.descriptor.placement_key_column !=
          request.current_descriptor.placement_key_column ||
      result.descriptor.policy_uuid != request.current_descriptor.policy_uuid ||
      result.descriptor.physical_clustering_enabled !=
          request.current_descriptor.physical_clustering_enabled ||
      result.descriptor.descriptor_generation !=
          request.current_descriptor.descriptor_generation;

  AddEvidence(&result, "ordered_ingest_physical_clustering", "controlled");
  AddEvidence(&result,
              "ordered_ingest_clustering_key_column",
              result.descriptor.placement_key_column);
  AddEvidence(&result,
              "ordered_ingest_clustering_descriptor_updated",
              result.descriptor_updated ? "true" : "false");
  AddEvidence(&result,
              "ordered_ingest_clustering_descriptor_generation",
              std::to_string(result.descriptor.descriptor_generation));
  AddEvidence(&result,
              "ordered_ingest_row_identity_rule",
              "engine_uuid_v7_only");
  return result;
}

}  // namespace scratchbird::storage::page
