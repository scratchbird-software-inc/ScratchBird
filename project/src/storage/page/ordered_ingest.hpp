// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {

struct OrderedIngestPhysicalClusteringDescriptor {
  std::string relation_uuid;
  std::string placement_key_column;
  std::string policy_uuid;
  std::uint64_t descriptor_generation = 0;
  bool physical_clustering_enabled = false;
};

struct OrderedIngestPhysicalClusteringRequest {
  OrderedIngestPhysicalClusteringDescriptor current_descriptor;
  std::string requested_placement_key_column;
  std::string requested_policy_uuid;
  bool ordered_ingest_selected = false;
  bool physical_clustering_requested = false;
  bool explicit_policy_present = false;
  bool allow_clustering_key_change = false;
};

struct OrderedIngestPhysicalClusteringResult {
  bool ok = true;
  bool descriptor_updated = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  OrderedIngestPhysicalClusteringDescriptor descriptor;
  std::vector<std::pair<std::string, std::string>> evidence;
};

OrderedIngestPhysicalClusteringResult ResolveOrderedIngestPhysicalClustering(
    const OrderedIngestPhysicalClusteringRequest& request);

}  // namespace scratchbird::storage::page
