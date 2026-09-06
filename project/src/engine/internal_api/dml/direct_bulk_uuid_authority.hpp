// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/insert_physical_integration.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_UUID_AUTHORITY
// Owns batched UUIDv7 allocation for direct physical bulk rows and versions.
// These identifiers are stable identity only and never transaction ordering or
// MGA finality authority.
struct DirectBulkUuidBatch {
  std::vector<std::string> row_uuids;
  std::vector<std::string> version_uuids;
  std::vector<std::string> row_image_uuids;
  std::size_t generated_row_uuids = 0;
  std::size_t caller_row_uuids = 0;
  std::size_t reservoir_served_uuids = 0;
  std::size_t reservoir_sync_generated_uuids = 0;
  bool reservoir_async_refill_requested = false;
  std::string batch_evidence_id;
};

DirectBulkUuidBatch BuildDirectBulkUuidBatch(
    const DirectPhysicalBulkAppendRequest& request,
    std::size_t row_count);
void AddDirectBulkUuidBatchEvidence(const DirectBulkUuidBatch& batch,
                                    DirectPhysicalBulkAppendResult* result);

}  // namespace scratchbird::engine::internal_api::dml::detail
