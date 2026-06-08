// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_STORAGE_VERSION_CLEANUP_AGENT
#include "transaction_cleanup_horizon_service.hpp"
#include "row_version.hpp"

#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;
using scratchbird::transaction::mga::RowVersionMetadata;

struct StorageRowVersionCleanupBatchMetrics {
  u64 total_row_versions = 0;
  u64 cleanup_candidate_row_versions = 0;
  u64 current_visible_row_versions = 0;
};

struct StorageRowVersionCleanupCandidateBatch {
  std::vector<RowVersionMetadata> candidates;
  StorageRowVersionCleanupBatchMetrics metrics;
  bool budget_exhausted = false;
};

StorageRowVersionCleanupCandidateBatch BuildStorageRowVersionCleanupCandidateBatch(
    const std::vector<RowVersionMetadata>& row_versions,
    const AuthoritativeCleanupHorizonResult& horizon,
    u64 max_candidate_row_versions);

bool StorageRowVersionHasCleanupShape(const RowVersionMetadata& metadata);
bool StorageRowVersionBelowCleanupHorizon(const RowVersionMetadata& metadata,
                                          const AuthoritativeCleanupHorizonResult& horizon);
bool StorageRowVersionIsCurrentVisibleVersion(const RowVersionMetadata& metadata);

}  // namespace scratchbird::storage::page
